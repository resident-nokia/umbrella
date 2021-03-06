/*
 * Copyright (c) 2016-2018, 2020 The Linux Foundation. All rights reserved.
 *
 * Permission to use, copy, modify, and/or distribute this software for
 * any purpose with or without fee is hereby granted, provided that the
 * above copyright notice and this permission notice appear in all
 * copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

/**
 * DOC: nan_datapath.c
 *
 * MAC NAN Data path API implementation
 */

#include "lim_utils.h"
#include "lim_api.h"
#include "lim_assoc_utils.h"
#include "nan_datapath.h"
#include "lim_types.h"
#include "lim_send_messages.h"
#include "wma_nan_datapath.h"
#include "os_if_nan.h"
#include "nan_public_structs.h"
#include "nan_ucfg_api.h"

/**
 * lim_add_ndi_peer() - Function to add ndi peer
 * @mac_ctx: handle to mac structure
 * @vdev_id: vdev id on which peer is added
 * @peer_mac_addr: peer to be added
 *
 * Return: QDF_STATUS_SUCCESS on success; error number otherwise
 */
static QDF_STATUS lim_add_ndi_peer(tpAniSirGlobal mac_ctx,
	uint32_t vdev_id, struct qdf_mac_addr peer_mac_addr)
{
	tpPESession session;
	tpDphHashNode sta_ds;
	uint16_t assoc_id, peer_idx;
	QDF_STATUS status;
	uint8_t zero_mac_addr[QDF_MAC_ADDR_SIZE] = { 0, 0, 0, 0, 0, 0 };

	if (!qdf_mem_cmp(&zero_mac_addr, &peer_mac_addr.bytes[0],
			QDF_MAC_ADDR_SIZE)) {
		pe_err("Failing to add peer with all zero mac addr");
		return QDF_STATUS_E_FAILURE;
	}

	session = pe_find_session_by_sme_session_id(mac_ctx,
						vdev_id);
	if (session == NULL) {
		/* couldn't find session */
		pe_err("Session not found for vdev_id: %d", vdev_id);
		return QDF_STATUS_E_FAILURE;
	}

	sta_ds = dph_lookup_hash_entry(mac_ctx,
				peer_mac_addr.bytes,
				&assoc_id, &session->dph.dphHashTable);
	/* peer exists, don't do anything */
	if (sta_ds != NULL) {
		pe_err("NDI Peer already exists!!");
		return QDF_STATUS_SUCCESS;
	}
	pe_info("Need to create NDI Peer :" MAC_ADDRESS_STR,
		MAC_ADDR_ARRAY(peer_mac_addr.bytes));

	peer_idx = lim_assign_peer_idx(mac_ctx, session);
	if (!peer_idx) {
		pe_err("Invalid peer_idx: %d", peer_idx);
		return QDF_STATUS_SUCCESS;
	}

	sta_ds = dph_add_hash_entry(mac_ctx, peer_mac_addr.bytes, peer_idx,
			&session->dph.dphHashTable);
	if (sta_ds == NULL) {
		pe_err("Couldn't add dph entry");
		/* couldn't add dph entry */
		return QDF_STATUS_E_FAILURE;
	}

	/* wma decides NDI mode from wma->inferface struct */
	sta_ds->staType = STA_ENTRY_NDI_PEER;
	status = lim_add_sta(mac_ctx, sta_ds, false, session);
	if (QDF_STATUS_SUCCESS != status) {
		/* couldn't add peer */
		pe_err("limAddSta failed status: %d",
			status);
		return QDF_STATUS_E_FAILURE;
	}

	return QDF_STATUS_SUCCESS;
}

QDF_STATUS lim_add_ndi_peer_converged(uint32_t vdev_id,
				struct qdf_mac_addr peer_mac_addr)
{
	tpAniSirGlobal mac_ctx = cds_get_context(QDF_MODULE_ID_PE);

	if (!mac_ctx)
		return QDF_STATUS_E_NULL_VALUE;

	return lim_add_ndi_peer(mac_ctx, vdev_id, peer_mac_addr);
}

/**
 * lim_ndp_delete_peer_by_addr() - Delete NAN data peer, given addr and vdev_id
 * @mac_ctx: handle to mac context
 * @vdev_id: vdev_id on which peer was added
 * @peer_ndi_mac_addr: mac addr of peer
 * This function deletes a peer if there was NDP_Confirm with REJECT
 *
 * Return: None
 */
static void lim_ndp_delete_peer_by_addr(tpAniSirGlobal mac_ctx, uint8_t vdev_id,
					struct qdf_mac_addr peer_ndi_mac_addr)
{
	tpPESession session;
	tpDphHashNode sta_ds;
	uint16_t peer_idx;
	uint8_t zero_mac_addr[QDF_MAC_ADDR_SIZE] = { 0, 0, 0, 0, 0, 0 };

	if (!qdf_mem_cmp(&zero_mac_addr, &peer_ndi_mac_addr.bytes[0],
			QDF_MAC_ADDR_SIZE)) {
		pe_err("Failing to delete the peer with all zero mac addr");
		return;
	}

	pe_info("deleting peer: "MAC_ADDRESS_STR" confirm rejected",
		MAC_ADDR_ARRAY(peer_ndi_mac_addr.bytes));

	session = pe_find_session_by_sme_session_id(mac_ctx, vdev_id);
	if (!session || (session->bssType != eSIR_NDI_MODE)) {
		pe_err("PE session is NULL or non-NDI for sme session %d",
			vdev_id);
		return;
	}

	sta_ds = dph_lookup_hash_entry(mac_ctx, peer_ndi_mac_addr.bytes,
				    &peer_idx, &session->dph.dphHashTable);
	if (!sta_ds) {
		pe_err("Unknown NDI Peer");
		return;
	}
	if (sta_ds->staType != STA_ENTRY_NDI_PEER) {
		pe_err("Non-NDI Peer ignored");
		return;
	}
	/*
	 * Call lim_del_sta() with response required set true. Hence
	 * DphHashEntry will be deleted after receiving that response.
	 */

	lim_del_sta(mac_ctx, sta_ds, true, session);
}

void lim_ndp_delete_peers_by_addr_converged(uint8_t vdev_id,
					struct qdf_mac_addr peer_ndi_mac_addr)
{
	tpAniSirGlobal mac_ctx = cds_get_context(QDF_MODULE_ID_PE);

	if (!mac_ctx)
		return;

	lim_ndp_delete_peer_by_addr(mac_ctx, vdev_id, peer_ndi_mac_addr);
}

/**
 * lim_ndp_delete_peers() - Delete NAN data peers
 * @mac_ctx: handle to mac context
 * @ndp_map: NDP instance/peer map
 * @num_peers: number of peers entries in peer_map
 * This function deletes a peer if there are no active NDPs left with that peer
 *
 * Return: None
 */
static void lim_ndp_delete_peers(tpAniSirGlobal mac_ctx,
				struct peer_ndp_map *ndp_map, uint8_t num_peers)
{
	tpDphHashNode sta_ds = NULL;
	uint16_t deleted_num = 0;
	int i, j;
	tpPESession session;
	struct qdf_mac_addr *deleted_peers;
	uint16_t peer_idx;
	bool found;

	deleted_peers = qdf_mem_malloc(num_peers * sizeof(*deleted_peers));
	if (!deleted_peers) {
		pe_err("Memory allocation failed");
		return;
	}

	for (i = 0; i < num_peers; i++) {
		pe_debug("ndp_map[%d]: MAC: " MAC_ADDRESS_STR " num_active %d",
			 i,
			 MAC_ADDR_ARRAY(ndp_map[i].peer_ndi_mac_addr.bytes),
			 ndp_map[i].num_active_ndp_sessions);

		/* Do not delete a peer with active NDPs */
		if (ndp_map[i].num_active_ndp_sessions > 0)
			continue;

		session = pe_find_session_by_sme_session_id(mac_ctx,
						ndp_map[i].vdev_id);
		if (!session || (session->bssType != eSIR_NDI_MODE)) {
			pe_err("PE session is NULL or non-NDI for sme session %d",
				ndp_map[i].vdev_id);
			continue;
		}

		/* Check if this peer is already in the deleted list */
		found = false;
		for (j = 0; j < deleted_num && !found; j++) {
			if (!qdf_mem_cmp(
				&deleted_peers[j].bytes,
				&ndp_map[i].peer_ndi_mac_addr.bytes,
				QDF_MAC_ADDR_SIZE)) {
				found = true;
				break;
			}
		}
		if (found)
			continue;

		sta_ds = dph_lookup_hash_entry(mac_ctx,
				ndp_map[i].peer_ndi_mac_addr.bytes,
				&peer_idx, &session->dph.dphHashTable);
		if (!sta_ds) {
			pe_err("Unknown NDI Peer");
			continue;
		}
		if (sta_ds->staType != STA_ENTRY_NDI_PEER) {
			pe_err("Non-NDI Peer ignored");
			continue;
		}
		/*
		 * Call lim_del_sta() with response required set true.
		 * Hence DphHashEntry will be deleted after receiving
		 * that response.
		 */
		lim_del_sta(mac_ctx, sta_ds, true, session);
		qdf_copy_macaddr(&deleted_peers[deleted_num++],
			&ndp_map[i].peer_ndi_mac_addr);
	}
	qdf_mem_free(deleted_peers);
}

void lim_ndp_delete_peers_converged(struct peer_nan_datapath_map *ndp_map,
				    uint8_t num_peers)
{
	tpAniSirGlobal mac_ctx = cds_get_context(QDF_MODULE_ID_PE);

	if (!mac_ctx)
		return;

	lim_ndp_delete_peers(mac_ctx, (struct peer_ndp_map *)ndp_map,
			     num_peers);
}

/**
 * lim_process_ndi_del_sta_rsp() - Handle WDA_DELETE_STA_RSP in eLIM_NDI_ROLE
 * @mac_ctx: Global MAC context
 * @lim_msg: LIM message
 * @pe_session: PE session
 *
 * Return: None
 */
void lim_process_ndi_del_sta_rsp(tpAniSirGlobal mac_ctx,
				 struct scheduler_msg *lim_msg,
				 tpPESession pe_session)
{
	tpDphHashNode sta_ds;
	tpDeleteStaParams del_sta_params = (tpDeleteStaParams) lim_msg->bodyptr;
	struct wlan_objmgr_vdev *vdev;
	struct wlan_objmgr_psoc *psoc = mac_ctx->psoc;
	struct nan_datapath_peer_ind peer_ind;

	if (!del_sta_params) {
		pe_err("del_sta_params is NULL");
		return;
	}
	if (!LIM_IS_NDI_ROLE(pe_session)) {
		pe_err("Session %d is not NDI role", del_sta_params->assocId);
		goto skip_event;
	}

	sta_ds = dph_get_hash_entry(mac_ctx, del_sta_params->assocId,
			&pe_session->dph.dphHashTable);
	if (!sta_ds) {
		pe_err("DPH Entry for STA %X is missing",
			del_sta_params->assocId);
		goto skip_event;
	}

	if (QDF_STATUS_SUCCESS != del_sta_params->status) {
		pe_err("DEL STA failed!");
		goto skip_event;
	}
	pe_info("Deleted STA AssocID %d staId %d MAC " MAC_ADDRESS_STR,
		sta_ds->assocId, sta_ds->staIndex,
		MAC_ADDR_ARRAY(sta_ds->staAddr));

	/*
	 * Copy peer info in del peer indication before
	 * lim_delete_dph_hash_entry is called as this will be lost.
	 */
	peer_ind.sta_id = sta_ds->staIndex;
	qdf_mem_copy(&peer_ind.peer_mac_addr.bytes,
		sta_ds->staAddr, sizeof(tSirMacAddr));
	lim_release_peer_idx(mac_ctx, sta_ds->assocId, pe_session);
	lim_delete_dph_hash_entry(mac_ctx, sta_ds->staAddr, sta_ds->assocId,
			pe_session);
	pe_session->limMlmState = eLIM_MLM_IDLE_STATE;

	vdev = wlan_objmgr_get_vdev_by_id_from_psoc(psoc,
						    pe_session->smeSessionId,
						    WLAN_NAN_ID);
	if (!vdev) {
		pe_err("Failed to get vdev from id");
		goto skip_event;
	}
	ucfg_nan_event_handler(psoc, vdev, NDP_PEER_DEPARTED, &peer_ind);
	wlan_objmgr_vdev_release_ref(vdev, WLAN_NAN_ID);

skip_event:
	qdf_mem_free(del_sta_params);
	lim_msg->bodyptr = NULL;
}

/**
 * lim_process_ndi_mlm_add_bss_rsp() - Process ADD_BSS response for NDI
 * @mac_ctx: Pointer to Global MAC structure
 * @lim_msgq: The MsgQ header, which contains the response buffer
 * @session_entry: PE session
 *
 * Return: None
 */
void lim_process_ndi_mlm_add_bss_rsp(tpAniSirGlobal mac_ctx,
				     struct scheduler_msg *lim_msgq,
				     tpPESession session_entry)
{
	tLimMlmStartCnf mlm_start_cnf;
	tpAddBssParams add_bss_params = (tpAddBssParams) lim_msgq->bodyptr;

	if (NULL == add_bss_params) {
		pe_err("Invalid body pointer in message");
		goto end;
	}
	pe_debug("Status %d", add_bss_params->status);
	if (QDF_STATUS_SUCCESS == add_bss_params->status) {
		pe_debug("WDA_ADD_BSS_RSP returned QDF_STATUS_SUCCESS");
		session_entry->limMlmState = eLIM_MLM_BSS_STARTED_STATE;
		MTRACE(mac_trace(mac_ctx, TRACE_CODE_MLM_STATE,
			session_entry->peSessionId,
			session_entry->limMlmState));
		session_entry->bssIdx = (uint8_t) add_bss_params->bssIdx;
		session_entry->limSystemRole = eLIM_NDI_ROLE;
		session_entry->statypeForBss = STA_ENTRY_SELF;
		session_entry->staId = add_bss_params->staContext.staIdx;
		/* Apply previously set configuration at HW */
		lim_apply_configuration(mac_ctx, session_entry);
		mlm_start_cnf.resultCode = eSIR_SME_SUCCESS;

		/* Initialize peer ID pool */
		lim_init_peer_idxpool(mac_ctx, session_entry);
	} else {
		pe_err("WDA_ADD_BSS_REQ failed with status %d",
			add_bss_params->status);
		mlm_start_cnf.resultCode = eSIR_SME_HAL_SEND_MESSAGE_FAIL;
	}
	mlm_start_cnf.sessionId = session_entry->peSessionId;
	lim_post_sme_message(mac_ctx, LIM_MLM_START_CNF,
				(uint32_t *) &mlm_start_cnf);
end:
	qdf_mem_free(lim_msgq->bodyptr);
	lim_msgq->bodyptr = NULL;
}

/**
 * lim_ndi_del_bss_rsp() - Handler for DEL BSS resp for NDI interface
 * @mac_ctx: handle to mac structure
 * @msg: pointer to message
 * @session_entry: session entry
 *
 * Return: None
 */
void lim_ndi_del_bss_rsp(tpAniSirGlobal  mac_ctx,
			void *msg, tpPESession session_entry)
{
	tSirResultCodes rc = eSIR_SME_SUCCESS;
	tpDeleteBssParams del_bss = (tpDeleteBssParams) msg;

	SET_LIM_PROCESS_DEFD_MESGS(mac_ctx, true);
	if (del_bss == NULL) {
		pe_err("NDI: DEL_BSS_RSP with no body!");
		rc = eSIR_SME_STOP_BSS_FAILURE;
		goto end;
	}
	session_entry =
		pe_find_session_by_session_id(mac_ctx, del_bss->sessionId);
	if (!session_entry) {
		pe_err("Session Does not exist for given sessionID");
		goto end;
	}

	if (del_bss->status != QDF_STATUS_SUCCESS) {
		pe_err("NDI: DEL_BSS_RSP error (%x) Bss %d",
			del_bss->status, del_bss->bssIdx);
		rc = eSIR_SME_STOP_BSS_FAILURE;
		goto end;
	}

	if (lim_set_link_state(mac_ctx, eSIR_LINK_IDLE_STATE,
			session_entry->selfMacAddr,
			session_entry->selfMacAddr, NULL, NULL)
			!= QDF_STATUS_SUCCESS) {
		pe_err("NDI: DEL_BSS_RSP setLinkState failed");
		goto end;
	}

	session_entry->limMlmState = eLIM_MLM_IDLE_STATE;

end:
	if (del_bss)
		qdf_mem_free(del_bss);
	/* Delete PE session once BSS is deleted */
	if (NULL != session_entry) {
		lim_send_sme_rsp(mac_ctx, eWNI_SME_STOP_BSS_RSP,
			rc, session_entry->smeSessionId,
			session_entry->transactionId);
		pe_delete_session(mac_ctx, session_entry);
		session_entry = NULL;
	}
}

static QDF_STATUS lim_send_sme_ndp_add_sta_rsp(tpAniSirGlobal mac_ctx,
						tpPESession session,
						tAddStaParams *add_sta_rsp)
{
	struct nan_datapath_peer_ind *new_peer_ind;
	struct wlan_objmgr_psoc *psoc = mac_ctx->psoc;
	struct wlan_objmgr_vdev *vdev;

	if (!add_sta_rsp) {
		pe_debug("Invalid add_sta_rsp");
		return QDF_STATUS_E_INVAL;
	}

	if (!psoc) {
		pe_debug("Invalid psoc");
		return QDF_STATUS_E_INVAL;
	}

	vdev = wlan_objmgr_get_vdev_by_id_from_psoc(psoc,
						    add_sta_rsp->smesessionId,
						    WLAN_NAN_ID);
	if (!vdev) {
		pe_err("Failed to get vdev from id");
		return QDF_STATUS_E_INVAL;
	}

	new_peer_ind = qdf_mem_malloc(sizeof(*new_peer_ind));
	if (!new_peer_ind) {
		pe_err("Failed to allocate memory");
		wlan_objmgr_vdev_release_ref(vdev, WLAN_NAN_ID);
		return QDF_STATUS_E_NOMEM;
	}

	qdf_mem_copy(new_peer_ind->peer_mac_addr.bytes, add_sta_rsp->staMac,
		     sizeof(tSirMacAddr));
	new_peer_ind->sta_id = add_sta_rsp->staIdx;

	ucfg_nan_event_handler(psoc, vdev, NDP_NEW_PEER, new_peer_ind);
	qdf_mem_free(new_peer_ind);
	wlan_objmgr_vdev_release_ref(vdev, WLAN_NAN_ID);
	return QDF_STATUS_SUCCESS;
}

/**
 * lim_ndp_add_sta_rsp() - handles add sta rsp for NDP from WMA
 * @mac_ctx: handle to mac structure
 * @session: session pointer
 * @add_sta_rsp: add sta response struct
 *
 * Return: None
 */
void lim_ndp_add_sta_rsp(tpAniSirGlobal mac_ctx, tpPESession session,
			 tAddStaParams *add_sta_rsp)
{
	tpDphHashNode sta_ds;
	uint16_t peer_idx;

	if (NULL == add_sta_rsp) {
		pe_err("Invalid add_sta_rsp");
		qdf_mem_free(add_sta_rsp);
		return;
	}

	SET_LIM_PROCESS_DEFD_MESGS(mac_ctx, true);
	sta_ds = dph_lookup_hash_entry(mac_ctx, add_sta_rsp->staMac, &peer_idx,
				    &session->dph.dphHashTable);
	if (sta_ds == NULL) {
		pe_err("NAN: ADD_STA_RSP for unknown MAC addr "
			MAC_ADDRESS_STR,
			MAC_ADDR_ARRAY(add_sta_rsp->staMac));
		qdf_mem_free(add_sta_rsp);
		return;
	}

	if (add_sta_rsp->status != QDF_STATUS_SUCCESS) {
		pe_err("NAN: ADD_STA_RSP error %x for MAC addr: %pM",
			add_sta_rsp->status, add_sta_rsp->staMac);
		/* delete the sta_ds allocated during ADD STA */
		lim_delete_dph_hash_entry(mac_ctx, add_sta_rsp->staMac,
				      peer_idx, session);
		qdf_mem_free(add_sta_rsp);
		return;
	}
	sta_ds->bssId = add_sta_rsp->bssIdx;
	sta_ds->staIndex = add_sta_rsp->staIdx;
	sta_ds->valid = 1;
	sta_ds->mlmStaContext.mlmState = eLIM_MLM_LINK_ESTABLISHED_STATE;
	lim_send_sme_ndp_add_sta_rsp(mac_ctx, session, add_sta_rsp);
	qdf_mem_free(add_sta_rsp);
}
