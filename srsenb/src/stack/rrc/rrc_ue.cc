/*
 * Copyright 2013-2020 Software Radio Systems Limited
 *
 * This file is part of srsLTE.
 *
 * srsLTE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * srsLTE is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * A copy of the GNU Affero General Public License can be found in
 * the LICENSE file in the top-level directory of this distribution
 * and at http://www.gnu.org/licenses/.
 *
 */

#include "srsenb/hdr/stack/rrc/rrc_ue.h"
#include "srslte/asn1/rrc_asn1_utils.h"
#include "srslte/interfaces/sched_interface.h"
#include "srslte/rrc/bearer_cfg.h"

namespace srsenb {

using namespace asn1::rrc;

bearer_handler::bearer_handler(uint16_t                   rnti_,
                               const rrc_cfg_t&           cfg_,
                               pdcp_interface_rrc*        pdcp_,
                               rlc_interface_rrc*         rlc_,
                               mac_interface_rrc*         mac_,
                               gtpu_interface_rrc*        gtpu_,
                               sched_interface::ue_cfg_t& ue_cfg_) :
  rnti(rnti_),
  cfg(&cfg_),
  pdcp(pdcp_),
  rlc(rlc_),
  mac(mac_),
  gtpu(gtpu_),
  sched_ue_cfg(&ue_cfg_)
{
  pool = srslte::byte_buffer_pool::get_instance();
}

void bearer_handler::setup_srb(uint8_t srb_id)
{
  if (srb_id > 2 or srb_id == 0) {
    log_h->error("Invalid SRB id=%d\n", srb_id);
    return;
  }

  // Set SRBtoAddMod
  auto srb_it               = srslte::add_rrc_obj_id(srbs_to_add, srb_id);
  srb_it->lc_ch_cfg_present = true;
  srb_it->lc_ch_cfg.set(srb_to_add_mod_s::lc_ch_cfg_c_::types_opts::default_value);
  srb_it->rlc_cfg_present = true;
  srb_it->rlc_cfg.set(srb_to_add_mod_s::rlc_cfg_c_::types_opts::default_value);
}

int bearer_handler::setup_erab(uint8_t                                            erab_id,
                               const asn1::s1ap::erab_level_qos_params_s&         qos,
                               const asn1::bounded_bitstring<1, 160, true, true>& addr,
                               uint32_t                                           teid_out,
                               const asn1::unbounded_octstring<true>*             nas_pdu)
{
  if (erab_id < 5) {
    log_h->error("ERAB id=%d is invalid\n", erab_id);
    return SRSLTE_ERROR;
  }
  uint8_t lcid  = erab_id - 2; // Map e.g. E-RAB 5 to LCID 3 (==DRB1)
  uint8_t drbid = erab_id - 4;

  if (qos.qci >= MAX_NOF_QCI) {
    log_h->error("Invalid QCI=%d for ERAB_id=%d, DRB_id=%d\n", qos.qci, erab_id, drbid);
    return SRSLTE_ERROR;
  }
  if (not cfg->qci_cfg[qos.qci].configured) {
    log_h->error("QCI=%d not configured\n", qos.qci);
    return SRSLTE_ERROR;
  }

  erabs[erab_id].id         = erab_id;
  erabs[erab_id].qos_params = qos;
  erabs[erab_id].address    = addr;
  erabs[erab_id].teid_out   = teid_out;

  if (addr.length() > 32) {
    log_h->error("Only addresses with length <= 32 are supported\n");
    return SRSLTE_ERROR;
  }

  if (nas_pdu != nullptr and nas_pdu->size() > 0) {
    erab_info_list[erab_id].assign(nas_pdu->data(), nas_pdu->data() + nas_pdu->size());
    log_h->info_hex(
        &erab_info_list[erab_id][0], erab_info_list[erab_id].size(), "setup_erab nas_pdu -> erab_info rnti 0x%x", rnti);
  }

  // Set DRBtoAddMod
  auto drb_it                                              = srslte::add_rrc_obj_id(drbs_to_add, drbid);
  drb_it->lc_ch_id_present                                 = true;
  drb_it->lc_ch_id                                         = (uint8_t)lcid;
  drb_it->eps_bearer_id_present                            = true;
  drb_it->eps_bearer_id                                    = (uint8_t)erab_id;
  drb_it->lc_ch_cfg_present                                = true;
  drb_it->lc_ch_cfg.ul_specific_params_present             = true;
  drb_it->lc_ch_cfg.ul_specific_params.lc_ch_group_present = true;
  drb_it->lc_ch_cfg.ul_specific_params                     = cfg->qci_cfg[qos.qci].lc_cfg;
  drb_it->pdcp_cfg_present                                 = true;
  drb_it->pdcp_cfg                                         = cfg->qci_cfg[qos.qci].pdcp_cfg;
  drb_it->rlc_cfg_present                                  = true;
  drb_it->rlc_cfg                                          = cfg->qci_cfg[qos.qci].rlc_cfg;

  // Initialize ERAB in GTPU right-away. DRBs are only created during RRC setup/reconf
  uint32_t addr_ = addr.to_number();
  gtpu->add_bearer(rnti, lcid, addr_, erabs[erab_id].teid_out, &(erabs[erab_id].teid_in));
  return SRSLTE_SUCCESS;
}

void bearer_handler::release_erab(uint8_t erab_id)
{
  auto it = erabs.find(erab_id);
  if (it == erabs.end()) {
    log_h->warning("The user rnti=0x%x does not contain ERAB-ID=%d\n", rnti, erab_id);
    return;
  }

  uint8_t drb_id = erab_id - 4;
  drbs_to_release.push_back(drb_id);

  erabs.erase(it);
  erab_info_list.erase(erab_id);
}

void bearer_handler::release_erabs()
{
  // TODO: notify GTPU layer for each ERAB
  erabs.clear();
  while (not erabs.empty()) {
    release_erab(erabs.begin()->first);
  }
}

void bearer_handler::handle_rrc_setup(asn1::rrc::rrc_conn_setup_r8_ies_s* msg)
{
  fill_and_apply_bearer_updates(msg->rr_cfg_ded);
}

void bearer_handler::handle_rrc_reest(asn1::rrc::rrc_conn_reest_r8_ies_s* msg)
{
  fill_and_apply_bearer_updates(msg->rr_cfg_ded);
}

void bearer_handler::handle_rrc_reconf(asn1::rrc::rrc_conn_recfg_r8_ies_s* msg)
{
  fill_and_apply_bearer_updates(msg->rr_cfg_ded);
  msg->rr_cfg_ded_present = msg->rr_cfg_ded.drb_to_add_mod_list_present or
                            msg->rr_cfg_ded.srb_to_add_mod_list_present or msg->rr_cfg_ded.drb_to_release_list_present;

  // Config RLC/PDCP
  fill_pending_nas_info(msg);
}

void bearer_handler::handle_rrc_reconf_complete()
{
  // Finally, add SRB2 and DRBs and any dedicated DRBs to the scheduler
  srsenb::sched_interface::ue_bearer_cfg_t bearer_cfg = {};
  for (const srb_to_add_mod_s& srb : srbs_to_add) {
    bearer_cfg.direction = srsenb::sched_interface::ue_bearer_cfg_t::BOTH;
    bearer_cfg.group     = 0;
    mac->bearer_ue_cfg(rnti, srb.srb_id, &bearer_cfg);
    sched_ue_cfg->ue_bearers[srb.srb_id] = bearer_cfg;
  }
  for (uint8_t drb_id : drbs_to_release) {
    bearer_cfg.direction = sched_interface::ue_bearer_cfg_t::IDLE;
    mac->bearer_ue_cfg(rnti, drb_id + 2, &bearer_cfg);
    sched_ue_cfg->ue_bearers[drb_id + 2] = bearer_cfg;
  }
  for (const drb_to_add_mod_s& drb : drbs_to_add) {
    bearer_cfg.direction = sched_interface::ue_bearer_cfg_t::BOTH;
    bearer_cfg.group     = drb.lc_ch_cfg.ul_specific_params.lc_ch_group;
    mac->bearer_ue_cfg(rnti, drb.lc_ch_id, &bearer_cfg);
    sched_ue_cfg->ue_bearers[drb.lc_ch_id] = bearer_cfg;
  }

  // Acknowledge Dedicated Configuration
  mac->phy_config_enabled(rnti, true);

  // Reset ToAdd state
  srbs_to_add = {};
  drbs_to_add = {};
  drbs_to_release.resize(0);
}

void bearer_handler::fill_and_apply_bearer_updates(asn1::rrc::rr_cfg_ded_s& msg)
{
  // Add altered bearers to message
  msg.srb_to_add_mod_list_present = srbs_to_add.size() > 0;
  msg.srb_to_add_mod_list         = srbs_to_add;
  msg.drb_to_add_mod_list_present = drbs_to_add.size() > 0;
  msg.drb_to_add_mod_list         = drbs_to_add;
  msg.drb_to_release_list_present = drbs_to_release.size() > 0;
  msg.drb_to_release_list         = drbs_to_release;

  // Apply changes in internal bearer_handler DRB/SRBtoAddModLists
  srslte::apply_srb_diff(last_srbs, msg, last_srbs);
  srslte::apply_drb_diff(last_drbs, msg, last_drbs);

  // Apply SRB updates to PDCP and RLC
  if (msg.srb_to_add_mod_list_present) {
    for (const srb_to_add_mod_s& srb : msg.srb_to_add_mod_list) {
      // Configure SRB1 in RLC
      rlc->add_bearer(rnti, srb.srb_id, srslte::rlc_config_t::srb_config(srb.srb_id));

      // Configure SRB1 in PDCP
      pdcp->add_bearer(rnti, srb.srb_id, srslte::make_srb_pdcp_config_t(srb.srb_id, false));
    }
  }

  // Apply DRB updates to PDCP and RLC
  if (msg.drb_to_release_list_present) {
    log_h->error("Removing DRBs not currently supported\n");
  }
  if (msg.drb_to_add_mod_list_present) {
    for (const drb_to_add_mod_s& drb : msg.drb_to_add_mod_list) {
      // Configure DRBs in RLC
      rlc->add_bearer(rnti, drb.lc_ch_id, srslte::make_rlc_config_t(drb.rlc_cfg));

      // Configure DRB1 in PDCP
      if (drb.pdcp_cfg_present) {
        srslte::pdcp_config_t pdcp_cnfg_drb = srslte::make_drb_pdcp_config_t(drb.drb_id, false, drb.pdcp_cfg);
        pdcp->add_bearer(rnti, drb.lc_ch_id, pdcp_cnfg_drb);
      } else {
        srslte::pdcp_config_t pdcp_cnfg_drb = srslte::make_drb_pdcp_config_t(drb.drb_id, false);
        pdcp->add_bearer(rnti, drb.lc_ch_id, pdcp_cnfg_drb);
      }
    }
  }
}

void bearer_handler::fill_pending_nas_info(asn1::rrc::rrc_conn_recfg_r8_ies_s* msg)
{
  // Add space for NAS messages
  uint8_t n_nas = erab_info_list.size();
  if (n_nas > 0) {
    msg->ded_info_nas_list_present = true;
    msg->ded_info_nas_list.resize(n_nas);
  }

  uint32_t idx = 0;
  // DRBs have already been configured in GTPU during bearer setup
  // Add E-RAB info message for the E-RABs
  if (msg->rr_cfg_ded.drb_to_add_mod_list_present) {
    for (const drb_to_add_mod_s& drb : msg->rr_cfg_ded.drb_to_add_mod_list) {
      uint8_t erab_id = drb.drb_id + 4;
      auto    it      = erab_info_list.find(erab_id);
      if (it != erab_info_list.end()) {
        const std::vector<uint8_t>& erab_info = it->second;
        log_h->info_hex(&erab_info[0], erab_info.size(), "connection_reconf erab_info -> nas_info rnti 0x%x\n", rnti);
        msg->ded_info_nas_list[idx].resize(erab_info.size());
        memcpy(msg->ded_info_nas_list[idx].data(), &erab_info[0], erab_info.size());
        erab_info_list.erase(it);
      } else {
        log_h->debug("Not adding NAS message to connection reconfiguration. E-RAB id %d\n", erab_id);
      }
      idx++;
    }
  }
}

} // namespace srsenb
