/********************************************************************
 * This file includes functions to read an OpenFPGA architecture file
 * which are built on the libarchopenfpga library
 *******************************************************************/
/* Headers from vtrutil library */
#include "vtr_time.h"
#include "vtr_assert.h"
#include "vtr_log.h"

#include "vpr_pb_type_annotation.h"
#include "pb_type_utils.h"
#include "openfpga_link_arch.h"

/* Include global variables of VPR */
#include "globals.h"

/* begin namespace openfpga */
namespace openfpga {

/********************************************************************
 * This function will traverse pb_type graph from its top to find
 * a pb_type with a given name as well as its hierarchy 
 *******************************************************************/
static 
t_pb_type* try_find_pb_type_with_given_path(t_pb_type* top_pb_type, 
                                            const std::vector<std::string>& target_pb_type_names, 
                                            const std::vector<std::string>& target_pb_mode_names) {
  /* Ensure that number of parent names and modes matches */
  VTR_ASSERT_SAFE(target_pb_type_names.size() == target_pb_mode_names.size() + 1);

  t_pb_type* cur_pb_type = top_pb_type;

  /* If the top pb_type is what we want, we can return here */
  if (1 == target_pb_type_names.size()) {
    if (target_pb_type_names[0] == std::string(top_pb_type->name)) {
      return top_pb_type;
    }
    /* Not match, return null pointer */
    return nullptr;
  }

  /* We start from the first element of the parent names and parent modes.
   * If the pb_type does not match in name, we fail 
   * If we cannot find a mode match the name, we fail 
   */
  for (size_t i = 0; i < target_pb_type_names.size() - 1; ++i) {
    /* If this level does not match, search fail */
    if (target_pb_type_names[i] != std::string(cur_pb_type->name)) {
      return nullptr;
    }
    /* Find if the mode matches */
    t_mode* cur_mode = find_pb_type_mode(cur_pb_type, target_pb_mode_names[i].c_str()); 
    if (nullptr == cur_mode) {
      return nullptr;
    }
    /* Go to the next level of pb_type */
    cur_pb_type = find_mode_child_pb_type(cur_mode, target_pb_type_names[i + 1].c_str());
    if (nullptr == cur_pb_type) {
      return nullptr;
    }
    /* If this is already the last pb_type in the list, this is what we want */
    if (i + 1 == target_pb_type_names.size() - 1) {
      return cur_pb_type;
    }
  }

  /* Reach here, it means we find nothing */
  return nullptr;
}

/********************************************************************
 * This function will identify the physical mode for each multi-mode 
 * pb_type in VPR pb_type graph by following the explicit definition 
 * in OpenFPGA architecture XML
 *******************************************************************/
static 
void build_vpr_physical_pb_mode_explicit_annotation(const DeviceContext& vpr_device_ctx, 
                                                    const Arch& openfpga_arch,
                                                    VprPbTypeAnnotation& vpr_pb_type_annotation) {
  /* Walk through the pb_type annotation stored in the openfpga arch */
  for (const PbTypeAnnotation& pb_type_annotation : openfpga_arch.pb_type_annotations) {
    /* Since our target is to annotate the physical mode name, 
     * we can skip those has not physical mode defined
     */
    if (true == pb_type_annotation.physical_mode_name().empty()) {
      continue;
    } 

    /* Identify if the pb_type is operating or physical,
     * For operating pb_type, get the full name of operating pb_type    
     * For physical pb_type, get the full name of physical pb_type    
     */
    std::vector<std::string> target_pb_type_names;
    std::vector<std::string> target_pb_mode_names;

    if (true == pb_type_annotation.is_operating_pb_type()) {
      target_pb_type_names = pb_type_annotation.operating_parent_pb_type_names();
      target_pb_type_names.push_back(pb_type_annotation.operating_pb_type_name());
      target_pb_mode_names = pb_type_annotation.operating_parent_mode_names();
    } 

    if (true == pb_type_annotation.is_physical_pb_type()) {
      target_pb_type_names = pb_type_annotation.physical_parent_pb_type_names();
      target_pb_type_names.push_back(pb_type_annotation.physical_pb_type_name());
      target_pb_mode_names = pb_type_annotation.physical_parent_mode_names();
    } 

    /* We must have at least one pb_type in the list */
    VTR_ASSERT_SAFE(0 < target_pb_type_names.size());

    /* Pb type information are located at the logic_block_types in the device context of VPR
     * We iterate over the vectors and find the pb_type matches the parent_pb_type_name
     */
    bool link_success = false;

    for (const t_logical_block_type& lb_type : vpr_device_ctx.logical_block_types) {
      /* By pass nullptr for pb_type head */
      if (nullptr == lb_type.pb_type) {
        continue;
      }
      /* Check the name of the top-level pb_type, if it does not match, we can bypass */
      if (target_pb_type_names[0] != std::string(lb_type.pb_type->name)) {
        continue;
      }
      /* Match the name in the top-level, we go further to search the pb_type in the graph */
      t_pb_type* target_pb_type = try_find_pb_type_with_given_path(lb_type.pb_type, target_pb_type_names, 
                                                                   target_pb_mode_names);
      if (nullptr == target_pb_type) {
        continue;
      }

      /* Found, we update the annotation by assigning the physical mode */
      t_mode* physical_mode = find_pb_type_mode(target_pb_type, pb_type_annotation.physical_mode_name().c_str());
      vpr_pb_type_annotation.add_pb_type_physical_mode(target_pb_type, physical_mode);
      
      /* Give a message */
      VTR_LOG("Annotate pb_type '%s' with physical mode '%s'\n",
              target_pb_type->name, physical_mode->name);

      link_success = true;
      break;
    }

    if (false == link_success) {
      /* Not found, error out! */
      VTR_LOG_ERROR("Unable to find the pb_type '%s' in VPR architecture definition!\n",
                    target_pb_type_names.back().c_str());
      return;
    }
  } 
}

/********************************************************************
 * This function will recursively visit all the pb_type from the top
 * pb_type in the graph and 
 * infer the physical mode for each multi-mode 
 * pb_type in VPR pb_type graph without OpenFPGA architecture XML
 *
 * The following rule is applied:
 * if there is only 1 mode under a pb_type, it will be the default 
 * physical mode for this pb_type
 *******************************************************************/
static 
void rec_infer_vpr_physical_pb_mode_annotation(t_pb_type* cur_pb_type, 
                                               VprPbTypeAnnotation& vpr_pb_type_annotation) {
  /* We do not check any primitive pb_type */
  if (true == is_primitive_pb_type(cur_pb_type)) {
    return;
  }

  /* For non-primitive pb_type:
   * - if there is only one mode, it will be the physical mode
   *   we just need to make sure that we do not repeatedly annotate this
   * - if there are multiple modes, we should be able to find a physical mode
   *   and then go recursively 
   */
  t_mode* physical_mode = nullptr;

  if (1 == cur_pb_type->num_modes) {
    if (nullptr == vpr_pb_type_annotation.physical_mode(cur_pb_type)) {
      /* Not assigned by explicit annotation, we should infer here */
      vpr_pb_type_annotation.add_pb_type_physical_mode(cur_pb_type, &(cur_pb_type->modes[0]));
      VTR_LOG("Implicitly infer physical mode '%s' for pb_type '%s'\n",
              cur_pb_type->modes[0].name, cur_pb_type->name);
    }
  } else {
    VTR_ASSERT(1 < cur_pb_type->num_modes);
    if (nullptr == vpr_pb_type_annotation.physical_mode(cur_pb_type)) {
      /* Not assigned by explicit annotation, we should infer here */
      vpr_pb_type_annotation.add_pb_type_physical_mode(cur_pb_type, &(cur_pb_type->modes[0]));
      VTR_LOG_ERROR("Unable to find a physical mode for a multi-mode pb_type '%s'!\n",
                    cur_pb_type->name);
      VTR_LOG_ERROR("Please specify in the OpenFPGA architecture\n");
      return;
    }
  }

  /* Get the physical mode from annotation */ 
  physical_mode = vpr_pb_type_annotation.physical_mode(cur_pb_type);

  VTR_ASSERT(nullptr != physical_mode);

  /* Traverse the pb_type children under the physical mode */
  for (int ichild = 0; ichild < physical_mode->num_pb_type_children; ++ichild) { 
    rec_infer_vpr_physical_pb_mode_annotation(&(physical_mode->pb_type_children[ichild]),
                                              vpr_pb_type_annotation);
  }
}

/********************************************************************
 * This function will infer the physical mode for each multi-mode 
 * pb_type in VPR pb_type graph without OpenFPGA architecture XML
 *
 * The following rule is applied:
 * if there is only 1 mode under a pb_type, it will be the default 
 * physical mode for this pb_type
 *
 * Note: 
 * This function must be executed AFTER the function
 *   build_vpr_physical_pb_mode_explicit_annotation()
 *******************************************************************/
static 
void build_vpr_physical_pb_mode_implicit_annotation(const DeviceContext& vpr_device_ctx, 
                                                    VprPbTypeAnnotation& vpr_pb_type_annotation) {
  for (const t_logical_block_type& lb_type : vpr_device_ctx.logical_block_types) {
    /* By pass nullptr for pb_type head */
    if (nullptr == lb_type.pb_type) {
      continue;
    }
    rec_infer_vpr_physical_pb_mode_annotation(lb_type.pb_type, vpr_pb_type_annotation); 
  }
}

/********************************************************************
 * This function will recursively traverse pb_type graph to ensure
 * 1. there is only a physical mode under each pb_type
 * 2. physical mode appears only when its parent is a physical mode.
 *******************************************************************/
static 
void rec_check_vpr_physical_pb_mode_annotation(t_pb_type* cur_pb_type,
                                               const bool& expect_physical_mode,
                                               const VprPbTypeAnnotation& vpr_pb_type_annotation,
                                               size_t& num_err) {
  /* We do not check any primitive pb_type */
  if (true == is_primitive_pb_type(cur_pb_type)) {
    return;
  }

  /* For non-primitive pb_type:
   * - If we expect a physical mode to exist under this pb_type
   *   we should be able to find one in the annoation 
   * - If we do NOT expect a physical mode, make sure we find 
   *   nothing in the annotation
   */
  if (true == expect_physical_mode) {
    if (nullptr == vpr_pb_type_annotation.physical_mode(cur_pb_type)) {
      VTR_LOG_ERROR("Unable to find a physical mode for a multi-mode pb_type '%s'!\n",
                    cur_pb_type->name);
      VTR_LOG_ERROR("Please specify in the OpenFPGA architecture\n");
      num_err++;
      return;
    }
  } else {
    VTR_ASSERT_SAFE(false == expect_physical_mode);
    if (nullptr != vpr_pb_type_annotation.physical_mode(cur_pb_type)) {
      VTR_LOG_ERROR("Find a physical mode '%s' for pb_type '%s' which is not under any physical mode!\n",
                    vpr_pb_type_annotation.physical_mode(cur_pb_type)->name,
                    cur_pb_type->name);
      num_err++;
      return;
    }
  }

  /* Traverse all the modes
   * - for pb_type children under a physical mode, we expect an physical mode 
   * - for pb_type children under non-physical mode, we expect no physical mode 
   */
  for (int imode = 0; imode < cur_pb_type->num_modes; ++imode) {
    bool expect_child_physical_mode = false;
    if (&(cur_pb_type->modes[imode]) == vpr_pb_type_annotation.physical_mode(cur_pb_type)) {
      expect_child_physical_mode = true && expect_physical_mode; 
    }
    for (int ichild = 0; ichild < cur_pb_type->modes[imode].num_pb_type_children; ++ichild) { 
      rec_check_vpr_physical_pb_mode_annotation(&(cur_pb_type->modes[imode].pb_type_children[ichild]),
                                                expect_child_physical_mode, vpr_pb_type_annotation,
                                                num_err);
    }
  }
}

/********************************************************************
 * This function will check the physical mode annotation for
 * each pb_type in the device
 *******************************************************************/
static 
void check_vpr_physical_pb_mode_annotation(const DeviceContext& vpr_device_ctx, 
                                           const VprPbTypeAnnotation& vpr_pb_type_annotation) {
  size_t num_err = 0;

  for (const t_logical_block_type& lb_type : vpr_device_ctx.logical_block_types) {
    /* By pass nullptr for pb_type head */
    if (nullptr == lb_type.pb_type) {
      continue;
    }
    /* Top pb_type should always has a physical mode! */
    rec_check_vpr_physical_pb_mode_annotation(lb_type.pb_type, true, vpr_pb_type_annotation, num_err);
  }
  if (0 == num_err) {
    VTR_LOG("Check physical mode annotation for pb_types passed.\n");
  } else {
    VTR_LOG("Check physical mode annotation for pb_types failed with %ld errors!\n",
            num_err);
  }
}

/********************************************************************
 * This function aims to make a pair of operating and physical 
 * pb_types:
 * - In addition to pairing the pb_types, it will pair the ports of the pb_types
 * - For the ports which are explicited annotated as physical pin mapping 
 *   in the pb_type annotation. 
 *   We will check the port range and create a pair 
 * - For the ports which are not specified in the pb_type annotation
 *   we assume their physical ports share the same as the operating ports
 *   We will try to find a port in the physical pb_type and check the port range
 *   If found, we will create a pair 
 * - All the pairs will be updated in vpr_pb_type_annotation
 *******************************************************************/
static 
bool pair_operating_and_physical_pb_types(t_pb_type* operating_pb_type, 
                                          t_pb_type* physical_pb_type,
                                          const PbTypeAnnotation& pb_type_annotation,
                                          VprPbTypeAnnotation& vpr_pb_type_annotation) {
  /* Reach here, we should have valid operating and physical pb_types */
  VTR_ASSERT((nullptr != operating_pb_type) && (nullptr != physical_pb_type));
  
  /* Iterate over the ports under the operating pb_type 
   * For each pin, we will try to find its physical port in the pb_type_annotation
   * if not found, we assume that the physical port is the same as the operating pb_port
   */
  for (t_port* operating_pb_port : pb_type_ports(operating_pb_type)) {
    /* Try to find the port in the pb_type_annotation */
    BasicPort expected_physical_pb_port = pb_type_annotation.physical_pb_type_port(std::string(operating_pb_port->name));
    if (true == expected_physical_pb_port.get_name().empty()) {
      /* Not found, we reset the port information to be consistent as the operating pb_port */
      expected_physical_pb_port.set_name(std::string(operating_pb_port->name));
      expected_physical_pb_port.set_width(operating_pb_port->num_pins);
    }

    /* Try to find the expected port in the physical pb_type */
    t_port* physical_pb_port = find_pb_type_port(physical_pb_type, expected_physical_pb_port.get_name());
    /* Not found, mapping fails */
    if (nullptr == physical_pb_port) {
      return false;
    }
    /* If the port range does not match, mapping fails */
    if (false == expected_physical_pb_port.contained(BasicPort(physical_pb_port->name, physical_pb_port->num_pins))) {
      return false;
    }
    /* Now, port mapping should succeed, we update the vpr_pb_type_annotation */
    vpr_pb_type_annotation.add_physical_pb_port(operating_pb_port, physical_pb_port);
    vpr_pb_type_annotation.add_physical_pb_port_range(operating_pb_port, expected_physical_pb_port);
  }

  /* Now, pb_type mapping should succeed, we update the vpr_pb_type_annotation */
  vpr_pb_type_annotation.add_physical_pb_type(operating_pb_type, physical_pb_type);

  return true;
}

/********************************************************************
 * This function will identify the physical pb_type for each operating 
 * pb_type in VPR pb_type graph by following the explicit definition 
 * in OpenFPGA architecture XML
 *
 * Note:
 * - This function should be executed only AFTER the physical mode 
 *   annotation is completed
 *******************************************************************/
static 
void build_vpr_physical_pb_type_annotation(const DeviceContext& vpr_device_ctx, 
                                           const Arch& openfpga_arch,
                                           VprPbTypeAnnotation& vpr_pb_type_annotation) {
  /* Walk through the pb_type annotation stored in the openfpga arch */
  for (const PbTypeAnnotation& pb_type_annotation : openfpga_arch.pb_type_annotations) {
    /* Since our target is to annotate the operating pb_type tp physical pb_type 
     * we can skip those annotation only for physical pb_type
     */
    if (true == pb_type_annotation.is_physical_pb_type()) {
      continue;
    }

    VTR_ASSERT(true == pb_type_annotation.is_operating_pb_type());

    /* Collect the information about the full hierarchy of operating pb_type to be annotated */
    std::vector<std::string> target_op_pb_type_names;
    std::vector<std::string> target_op_pb_mode_names;

    target_op_pb_type_names = pb_type_annotation.operating_parent_pb_type_names();
    target_op_pb_type_names.push_back(pb_type_annotation.operating_pb_type_name());
    target_op_pb_mode_names = pb_type_annotation.operating_parent_mode_names();

    /* Collect the information about the full hierarchy of physical pb_type to be annotated */
    std::vector<std::string> target_phy_pb_type_names;
    std::vector<std::string> target_phy_pb_mode_names;

    target_phy_pb_type_names = pb_type_annotation.physical_parent_pb_type_names();
    target_phy_pb_type_names.push_back(pb_type_annotation.physical_pb_type_name());
    target_phy_pb_mode_names = pb_type_annotation.physical_parent_mode_names();

    /* We must have at least one pb_type in the list */
    VTR_ASSERT_SAFE(0 < target_op_pb_type_names.size());
    VTR_ASSERT_SAFE(0 < target_phy_pb_type_names.size());

    /* Pb type information are located at the logic_block_types in the device context of VPR
     * We iterate over the vectors and find the pb_type matches the parent_pb_type_name
     */
    bool link_success = false;

    for (const t_logical_block_type& lb_type : vpr_device_ctx.logical_block_types) {
      /* By pass nullptr for pb_type head */
      if (nullptr == lb_type.pb_type) {
        continue;
      }
      /* Check the name of the top-level pb_type, if it does not match, we can bypass */
      if (target_op_pb_type_names[0] != std::string(lb_type.pb_type->name)) {
        continue;
      }
      /* Match the name in the top-level, we go further to search the operating as well as
       * physical pb_types in the graph */
      t_pb_type* target_op_pb_type = try_find_pb_type_with_given_path(lb_type.pb_type, target_op_pb_type_names, 
                                                                      target_op_pb_mode_names);
      if (nullptr == target_op_pb_type) {
        continue;
      }

      t_pb_type* target_phy_pb_type = try_find_pb_type_with_given_path(lb_type.pb_type, target_phy_pb_type_names, 
                                                                       target_phy_pb_mode_names);
      if (nullptr == target_phy_pb_type) {
        continue;
      }

      /* Both operating and physical pb_type have been found, 
       * we update the annotation by assigning the physical mode 
       */
      if (true == pair_operating_and_physical_pb_types(target_op_pb_type, target_phy_pb_type,
                                                       pb_type_annotation, vpr_pb_type_annotation)) {
      
        /* Give a message */
        VTR_LOG("Annotate operating pb_type '%s' to its physical pb_type '%s'\n",
                target_op_pb_type->name, target_phy_pb_type->name);

        link_success = true;
        break;
      }
    }

    if (false == link_success) {
      /* Not found, error out! */
      VTR_LOG_ERROR("Unable to pair the operating pb_type '%s' to its physical pb_type '%s'!\n",
                    target_op_pb_type_names.back().c_str(),
                    target_phy_pb_type_names.back().c_str());
      return;
    }
  } 
}


/********************************************************************
 * Top-level function to link openfpga architecture to VPR, including:
 * - physical pb_type
 * - idle pb_type 
 * - circuit models for pb_type, pb interconnect and routing architecture
 *******************************************************************/
void link_arch(OpenfpgaContext& openfpga_context) {

  vtr::ScopedStartFinishTimer timer("Link OpenFPGA architecture to VPR architecture");

  /* Annotate physical mode to pb_type in the VPR pb_type graph */
  build_vpr_physical_pb_mode_explicit_annotation(g_vpr_ctx.device(), openfpga_context.arch(),
                                                 openfpga_context.mutable_vpr_pb_type_annotation());
  build_vpr_physical_pb_mode_implicit_annotation(g_vpr_ctx.device(), 
                                                 openfpga_context.mutable_vpr_pb_type_annotation());

  check_vpr_physical_pb_mode_annotation(g_vpr_ctx.device(), 
                                        openfpga_context.vpr_pb_type_annotation());

  /* Annotate physical pb_types to operating pb_type in the VPR pb_type graph */
  build_vpr_physical_pb_type_annotation(g_vpr_ctx.device(), openfpga_context.arch(),
                                        openfpga_context.mutable_vpr_pb_type_annotation());

  /* Link physical pb_type to circuit model */

  /* Link routing architecture to circuit model */
} 

} /* end namespace openfpga */
