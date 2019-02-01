// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use failure::Fail;
use fidl_fuchsia_wlan_common as fidl_common;
use fidl_fuchsia_wlan_mlme::{self as fidl_mlme, BssDescription, ScanRequest, ScanResultCodes};
use log::error;
use std::collections::HashSet;
use std::mem;
use std::sync::Arc;

use crate::client::{DeviceInfo, Ssid};

const PASSIVE_SCAN_MIN_CHANNEL_MS: u32 = 100;
const PASSIVE_SCAN_MAX_CHANNEL_MS: u32 = 200;
const ACTIVE_SCAN_PROBE_DELAY_MS: u32 = 5;
const ACTIVE_SCAN_CHANNEL_MS: u32 = 50;

// Scans can be performed for two different purposes:
//      1) Discover available wireless networks. These scans are initiated by the "user",
//         i.e. a client process that talks to SME, and thus have 'tokens' to identify
//         a specific user request.
//      2) Join a specific network. These scans can only be initiated by the SME state machine
//         itself.

// An SME-initiated scan request for the purpose of joining a network
#[derive(Debug, PartialEq)]
pub struct JoinScan<T> {
    pub ssid: Ssid,
    pub token: T,
    pub scan_type: fidl_common::ScanType,
}

// A "user"-initiated scan request for the purpose of discovering available networks
#[derive(Debug, PartialEq)]
pub struct DiscoveryScan<T> {
    tokens: Vec<T>,
    scan_type: fidl_common::ScanType,
}

impl<T> DiscoveryScan<T> {
    pub fn new(token: T, scan_type: fidl_common::ScanType) -> Self {
        Self { tokens: vec![token], scan_type }
    }

    pub fn matches(&self, scan: &DiscoveryScan<T>) -> bool {
        self.scan_type == scan.scan_type
    }

    pub fn merges(&mut self, mut scan: DiscoveryScan<T>) {
        self.tokens.append(&mut scan.tokens)
    }
}

pub struct ScanScheduler<D, J> {
    // The currently running scan. We assume that MLME can handle a single concurrent scan
    // regardless of its own state.
    current: ScanState<D, J>,
    // A pending scan request for the purpose of joining a network. This type of request
    // always takes priority over discovery scans. There can only be one such pending request:
    // if SME requests another one, the newer one wins and overwrites an existing request.
    pending_join: Option<JoinScan<J>>,
    // Pending discovery requests from the user
    pending_discovery: Vec<DiscoveryScan<D>>,
    device_info: Arc<DeviceInfo>,
    last_mlme_txn_id: u64,
}

#[derive(Debug, PartialEq)]
enum ScanState<D, J> {
    NotScanning,
    ScanningToJoin { cmd: JoinScan<J>, mlme_txn_id: u64, bss_list: Vec<BssDescription> },
    ScanningToDiscover { cmd: DiscoveryScan<D>, mlme_txn_id: u64, bss_list: Vec<BssDescription> },
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum JoinScanFailure {
    // MLME returned an error code
    ScanFailed(ScanResultCodes),
    // Scan was canceled or superseded by another request
    Canceled,
}

// A reaction to MLME's ScanConfirm event
pub enum ScanResult<D, J> {
    // No reaction: the scan results are not relevant anymore, or we received
    // an unexpected ScanConfirm.
    None,
    // "Join" scan has finished, either successfully or not.
    // The SME state machine can now send a JoinRequest to MLME if it desires so.
    JoinScanFinished { token: J, result: JoinScanResult },
    // "Discovery" scan has finished, either successfully or not.
    // SME is expected to forward the result to the user.
    DiscoveryFinished { tokens: Vec<D>, result: DiscoveryResult },
}

pub type JoinScanResult = Result<Vec<BssDescription>, JoinScanFailure>;
pub type DiscoveryResult = Result<Vec<BssDescription>, DiscoveryError>;

#[derive(Clone, Debug, Fail)]
pub enum DiscoveryError {
    #[fail(display = "Scanning not supported by device")]
    NotSupported,
    #[fail(display = "Internal error occurred")]
    InternalError,
}

impl<D, J> ScanScheduler<D, J> {
    pub fn new(device_info: Arc<DeviceInfo>) -> Self {
        ScanScheduler {
            current: ScanState::NotScanning,
            pending_join: None,
            pending_discovery: Vec::new(),
            device_info,
            last_mlme_txn_id: 0,
        }
    }

    // Initiate a "join" scan.
    //
    // If there is an existing pending "join" scan request, it will be discarded and replaced
    // with the new one. In this case, the token of the discarded request will be returned.
    //
    // If there is a currently running "join" scan, its results will be discarded, too.
    //
    // The scan might or might not begin immediately.
    // If a ScanRequest is returned, the caller is responsible for forwarding it to MLME.
    pub fn enqueue_scan_to_join(&mut self, s: JoinScan<J>) -> (Option<J>, Option<ScanRequest>) {
        let old_token = mem::replace(&mut self.pending_join, Some(s)).map(|p| p.token);
        (old_token, self.start_next_scan())
    }

    // Initiate a "discovery" scan. The scan might or might not begin immediately.
    // The request can be merged with any pending or ongoing requests.
    // If a ScanRequest is returned, the caller is responsible for forwarding it to MLME.
    pub fn enqueue_scan_to_discover(&mut self, s: DiscoveryScan<D>) -> Option<ScanRequest> {
        if let ScanState::ScanningToDiscover { cmd, .. } = &mut self.current {
            if cmd.matches(&s) {
                cmd.merges(s);
                return None;
            }
        }
        if let Some(scan_cmd) = self.pending_discovery.iter_mut().find(|cmd| cmd.matches(&s)) {
            scan_cmd.merges(s);
            return None;
        }
        self.pending_discovery.push(s);
        self.start_next_scan()
    }

    // Should be called for every OnScanResult event received from MLME.
    pub fn on_mlme_scan_result(&mut self, msg: fidl_mlme::ScanResult) {
        if !self.matching_mlme_txn_id(msg.txn_id) {
            return;
        }
        match &mut self.current {
            ScanState::NotScanning => {}
            ScanState::ScanningToJoin { cmd, bss_list, .. } => {
                if cmd.ssid == msg.bss.ssid {
                    bss_list.push(msg.bss)
                }
            }
            ScanState::ScanningToDiscover { bss_list, .. } => bss_list.push(msg.bss),
        }
    }

    // Should be called for every OnScanEnd event received from MLME.
    // The caller is expected to take action based on the returned ScanResult.
    // If a ScanRequest is returned, the caller is responsible for forwarding it to MLME.
    pub fn on_mlme_scan_end(
        &mut self,
        msg: fidl_mlme::ScanEnd,
    ) -> (ScanResult<D, J>, Option<ScanRequest>) {
        if !self.matching_mlme_txn_id(msg.txn_id) {
            return (ScanResult::None, None);
        }
        let old_state = mem::replace(&mut self.current, ScanState::NotScanning);
        let result = match old_state {
            ScanState::NotScanning => ScanResult::None,
            ScanState::ScanningToJoin { cmd, bss_list, .. } => {
                let result = if self.pending_join.is_some() {
                    // The scan that just finished was superseded by a newer join scan request
                    Err(JoinScanFailure::Canceled)
                } else {
                    match msg.code {
                        ScanResultCodes::Success => Ok(bss_list),
                        other => Err(JoinScanFailure::ScanFailed(other)),
                    }
                };
                ScanResult::JoinScanFinished { token: cmd.token, result }
            }
            ScanState::ScanningToDiscover { cmd, bss_list, .. } => ScanResult::DiscoveryFinished {
                tokens: cmd.tokens,
                result: convert_discovery_result(msg, bss_list),
            },
        };
        let request = self.start_next_scan();
        (result, request)
    }

    // Returns the most recent join scan request, if there is one.
    pub fn get_join_scan(&self) -> Option<&JoinScan<J>> {
        if let Some(s) = &self.pending_join {
            Some(s)
        } else if let ScanState::ScanningToJoin { cmd, .. } = &self.current {
            Some(cmd)
        } else {
            None
        }
    }

    fn matching_mlme_txn_id(&self, incoming_txn_id: u64) -> bool {
        match &self.current {
            ScanState::NotScanning => false,
            ScanState::ScanningToJoin { mlme_txn_id, .. }
            | ScanState::ScanningToDiscover { mlme_txn_id, .. } => *mlme_txn_id == incoming_txn_id,
        }
    }

    fn start_next_scan(&mut self) -> Option<ScanRequest> {
        match &self.current {
            ScanState::NotScanning => {
                if let Some(join_scan) = self.pending_join.take() {
                    self.last_mlme_txn_id += 1;
                    let request =
                        new_join_scan_request(self.last_mlme_txn_id, &join_scan, &self.device_info);
                    self.current = ScanState::ScanningToJoin {
                        cmd: join_scan,
                        mlme_txn_id: self.last_mlme_txn_id,
                        bss_list: Vec::new(),
                    };
                    Some(request)
                } else if !self.pending_discovery.is_empty() {
                    self.last_mlme_txn_id += 1;
                    let scan_cmd = self.pending_discovery.remove(0);
                    let request = new_discovery_scan_request(
                        self.last_mlme_txn_id,
                        &scan_cmd,
                        &self.device_info,
                    );
                    self.current = ScanState::ScanningToDiscover {
                        cmd: scan_cmd,
                        mlme_txn_id: self.last_mlme_txn_id,
                        bss_list: Vec::new(),
                    };
                    Some(request)
                } else {
                    None
                }
            }
            _ => None,
        }
    }
}

const WILDCARD_BSS_ID: [u8; 6] = [0xff, 0xff, 0xff, 0xff, 0xff, 0xff];

fn new_scan_request(
    mlme_txn_id: u64,
    scan_type: fidl_common::ScanType,
    ssid: Vec<u8>,
    device_info: &DeviceInfo,
) -> ScanRequest {
    let scan_req = ScanRequest {
        txn_id: mlme_txn_id,
        bss_type: fidl_mlme::BssTypes::Infrastructure,
        bssid: WILDCARD_BSS_ID.clone(),
        ssid,
        scan_type: fidl_mlme::ScanTypes::Passive,
        probe_delay: 0,
        channel_list: Some(get_channels_to_scan(&device_info)),
        min_channel_time: PASSIVE_SCAN_MIN_CHANNEL_MS,
        max_channel_time: PASSIVE_SCAN_MAX_CHANNEL_MS,
        ssid_list: None,
    };
    match scan_type {
        fidl_common::ScanType::Active => ScanRequest {
            scan_type: fidl_mlme::ScanTypes::Active,
            probe_delay: ACTIVE_SCAN_PROBE_DELAY_MS,
            min_channel_time: ACTIVE_SCAN_CHANNEL_MS,
            max_channel_time: ACTIVE_SCAN_CHANNEL_MS,
            ..scan_req
        },
        fidl_common::ScanType::Passive => scan_req,
    }
}

fn new_join_scan_request<T>(
    mlme_txn_id: u64,
    join_scan: &JoinScan<T>,
    device_info: &DeviceInfo,
) -> ScanRequest {
    new_scan_request(mlme_txn_id, join_scan.scan_type, join_scan.ssid.clone(), device_info)
}

fn new_discovery_scan_request<T>(
    mlme_txn_id: u64,
    discovery_scan: &DiscoveryScan<T>,
    device_info: &DeviceInfo,
) -> ScanRequest {
    new_scan_request(mlme_txn_id, discovery_scan.scan_type, vec![], device_info)
}

fn convert_discovery_result(
    msg: fidl_mlme::ScanEnd,
    bss_list: Vec<BssDescription>,
) -> DiscoveryResult {
    match msg.code {
        ScanResultCodes::Success => Ok(bss_list),
        ScanResultCodes::NotSupported => Err(DiscoveryError::NotSupported),
        ScanResultCodes::InvalidArgs => {
            error!("Scan returned INVALID_ARGS");
            Err(DiscoveryError::InternalError)
        }
        ScanResultCodes::InternalError => Err(DiscoveryError::InternalError),
    }
}

fn get_channels_to_scan(device_info: &DeviceInfo) -> Vec<u8> {
    let mut device_supported_channels: HashSet<u8> = HashSet::new();
    for band in &device_info.bands {
        device_supported_channels.extend(&band.channels);
    }

    SUPPORTED_CHANNELS
        .iter()
        .filter(|chan| device_supported_channels.contains(chan))
        .map(|chan| *chan)
        .collect()
}

const SUPPORTED_CHANNELS: &[u8] = &[
    // 5GHz UNII-1
    36, 40, 44, 48, // 5GHz UNII-2 Middle
    52, 56, 60, 64, // 5GHz UNII-2 Extended
    100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144, // 5GHz UNII-3
    149, 153, 157, 161, 165, // 2GHz
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11,
];

#[cfg(test)]
mod tests {
    use super::*;

    use crate::client::test_utils::{fake_bss_with_bssid, fake_unprotected_bss_description};
    use crate::clone_utils::clone_bss_desc;

    const CLIENT_ADDR: [u8; 6] = [0x7A, 0xE7, 0x76, 0xD9, 0xF2, 0x67];

    fn passive_discovery_scan(token: i32) -> DiscoveryScan<i32> {
        DiscoveryScan::new(token, fidl_common::ScanType::Passive)
    }

    #[test]
    fn discovery_scan() {
        let mut sched = create_sched();
        let req = sched
            .enqueue_scan_to_discover(passive_discovery_scan(10))
            .expect("expected a ScanRequest");
        let txn_id = req.txn_id;
        sched.on_mlme_scan_result(fidl_mlme::ScanResult {
            txn_id,
            bss: fake_unprotected_bss_description(b"foo".to_vec()),
        });
        sched.on_mlme_scan_result(fidl_mlme::ScanResult {
            txn_id: txn_id + 100, // mismatching transaction id
            bss: fake_unprotected_bss_description(b"bar".to_vec()),
        });
        sched.on_mlme_scan_result(fidl_mlme::ScanResult {
            txn_id,
            bss: fake_unprotected_bss_description(b"qux".to_vec()),
        });
        let (result, req) = sched.on_mlme_scan_end(fidl_mlme::ScanEnd {
            txn_id,
            code: fidl_mlme::ScanResultCodes::Success,
        });
        assert!(req.is_none());
        let result = match result {
            ScanResult::DiscoveryFinished { tokens, result } => {
                assert_eq!(vec![10], tokens);
                result
            }
            _ => panic!("expected ScanResult::DiscoveryFinished"),
        };
        let mut ssid_list = result
            .expect("expected a successful scan result")
            .into_iter()
            .map(|bss| bss.ssid)
            .collect::<Vec<_>>();
        ssid_list.sort();
        assert_eq!(vec![b"foo".to_vec(), b"qux".to_vec()], ssid_list);
    }

    #[test]
    fn test_passive_discovery_scan_args() {
        let mut sched = create_sched();
        let req = sched
            .enqueue_scan_to_discover(passive_discovery_scan(10))
            .expect("expected a ScanRequest");

        assert_eq!(req.scan_type, fidl_mlme::ScanTypes::Passive);
        assert_eq!(req.ssid, vec![]);
    }

    #[test]
    fn test_active_discovery_scan_args() {
        let mut sched = create_sched();
        let scan_cmd = DiscoveryScan::new(10, fidl_common::ScanType::Active);
        let req = sched.enqueue_scan_to_discover(scan_cmd).expect("expected a ScanRequest");

        assert_eq!(req.scan_type, fidl_mlme::ScanTypes::Active);
        assert_eq!(req.ssid, vec![]);
    }

    #[test]
    fn test_discovery_scans_dedupe_single_group() {
        let mut sched = create_sched();

        // Post one scan command, expect a message to MLME
        let mlme_req = sched
            .enqueue_scan_to_discover(passive_discovery_scan(10))
            .expect("expected a ScanRequest");
        let txn_id = mlme_req.txn_id;

        // Report a scan result
        sched.on_mlme_scan_result(fidl_mlme::ScanResult {
            txn_id,
            bss: fake_unprotected_bss_description(b"foo".to_vec()),
        });

        // Post another command. It should not issue another request to the MLME since
        // there is already an on-going one
        assert!(sched.enqueue_scan_to_discover(passive_discovery_scan(20)).is_none());

        // Report another scan result and the end of the scan transaction
        sched.on_mlme_scan_result(fidl_mlme::ScanResult {
            txn_id,
            bss: fake_unprotected_bss_description(b"bar".to_vec()),
        });
        let (result, req) = sched.on_mlme_scan_end(fidl_mlme::ScanEnd {
            txn_id,
            code: fidl_mlme::ScanResultCodes::Success,
        });

        // We don't expect another request to the MLME
        assert!(req.is_none());

        // Expect a discovery result with both tokens and both SSIDs
        assert_discovery_scan_result(result, vec![10, 20], vec![b"bar".to_vec(), b"foo".to_vec()]);
    }

    #[test]
    fn test_discovery_scans_dedupe_multiple_groups() {
        let mut sched = create_sched();

        // Post a passive scan command, expect a message to MLME
        let mlme_req = sched
            .enqueue_scan_to_discover(passive_discovery_scan(10))
            .expect("expected a ScanRequest");
        let txn_id = mlme_req.txn_id;

        // Post an active scan command, which should be enqueued until the previous one finishes
        let scan_cmd = DiscoveryScan::new(20, fidl_common::ScanType::Active);
        assert!(sched.enqueue_scan_to_discover(scan_cmd).is_none());

        // Post a passive scan command. It should be merged with the ongoing one and so should not
        // issue another request to MLME
        assert!(sched.enqueue_scan_to_discover(passive_discovery_scan(30)).is_none());

        // Post an active scan command. It should be merged with the active scan command that's
        // still enqueued, and so should not issue another request to MLME
        let scan_cmd = DiscoveryScan::new(40, fidl_common::ScanType::Active);
        assert!(sched.enqueue_scan_to_discover(scan_cmd).is_none());

        // Report scan result and scan end
        sched.on_mlme_scan_result(fidl_mlme::ScanResult {
            txn_id,
            bss: fake_unprotected_bss_description(b"foo".to_vec()),
        });
        let (result, mlme_req) = sched.on_mlme_scan_end(fidl_mlme::ScanEnd {
            txn_id,
            code: fidl_mlme::ScanResultCodes::Success,
        });

        // Expect discovery result with 1st and 3rd tokens
        assert_discovery_scan_result(result, vec![10, 30], vec![b"foo".to_vec()]);

        // Next mlme_req should be an active scan request
        assert!(mlme_req.is_some());
        let mlme_req = mlme_req.unwrap();
        assert_eq!(mlme_req.scan_type, fidl_mlme::ScanTypes::Active);
        let txn_id = mlme_req.txn_id;

        // Report scan result and scan end
        sched.on_mlme_scan_result(fidl_mlme::ScanResult {
            txn_id,
            bss: fake_unprotected_bss_description(b"bar".to_vec()),
        });
        let (result, mlme_req) = sched.on_mlme_scan_end(fidl_mlme::ScanEnd {
            txn_id,
            code: fidl_mlme::ScanResultCodes::Success,
        });

        // Expect discovery result with 2nd and 3rd tokens
        assert_discovery_scan_result(result, vec![20, 40], vec![b"bar".to_vec()]);

        // We don't expect another request to the MLME
        assert!(mlme_req.is_none());
    }

    fn assert_discovery_scan_result(
        result: ScanResult<i32, i32>,
        expected_tokens: Vec<i32>,
        expected_ssids: Vec<Vec<u8>>,
    ) {
        let result = match result {
            ScanResult::DiscoveryFinished { tokens, result } => {
                assert_eq!(tokens, expected_tokens);
                result
            }
            _ => panic!("expected ScanResult::DiscoveryFinished"),
        };
        let mut ssid_list = result
            .expect("expected a successful scan result")
            .into_iter()
            .map(|bss| bss.ssid)
            .collect::<Vec<_>>();
        ssid_list.sort();
        assert_eq!(ssid_list, expected_ssids);
    }

    #[test]
    fn join_scan() {
        let mut sched = create_sched();
        let (discarded_token, req) = sched.enqueue_scan_to_join(JoinScan {
            ssid: b"foo".to_vec(),
            token: 10,
            scan_type: fidl_common::ScanType::Passive,
        });
        assert!(discarded_token.is_none());
        let txn_id = req.expect("expected a ScanRequest").txn_id;

        // Matching BSS
        let bss1 = fake_bss_with_bssid(b"foo".to_vec(), [1, 1, 1, 1, 1, 1]);
        sched.on_mlme_scan_result(fidl_mlme::ScanResult { txn_id, bss: clone_bss_desc(&bss1) });

        // Mismatching transaction ID
        let bss2 = fake_bss_with_bssid(b"foo".to_vec(), [2, 2, 2, 2, 2, 2]);
        sched.on_mlme_scan_result(fidl_mlme::ScanResult { txn_id: txn_id + 100, bss: bss2 });

        // Mismatching SSID
        let bss3 = fake_bss_with_bssid(b"bar".to_vec(), [3, 3, 3, 3, 3, 3]);
        sched.on_mlme_scan_result(fidl_mlme::ScanResult { txn_id, bss: bss3 });

        // Matching BSS
        let bss4 = fake_bss_with_bssid(b"foo".to_vec(), [4, 4, 4, 4, 4, 4]);
        sched.on_mlme_scan_result(fidl_mlme::ScanResult { txn_id, bss: clone_bss_desc(&bss4) });

        let (result, req) = sched.on_mlme_scan_end(fidl_mlme::ScanEnd {
            txn_id,
            code: fidl_mlme::ScanResultCodes::Success,
        });
        assert!(req.is_none());
        match result {
            ScanResult::JoinScanFinished { token, result } => {
                assert_eq!(10, token);
                assert_eq!(result, Ok(vec![bss1, bss4]));
            }
            _ => panic!("expected ScanResult::ReadyToJoin"),
        };
    }

    fn passive_join_scan(ssid: Vec<u8>, token: i32) -> JoinScan<i32> {
        JoinScan { ssid, token, scan_type: fidl_common::ScanType::Passive }
    }

    #[test]
    fn test_passive_join_scan_args() {
        let mut sched = create_sched();
        let (_discarded_token, req) =
            sched.enqueue_scan_to_join(passive_join_scan(b"foo".to_vec(), 10));

        let req = req.expect("expected a ScanRequest");
        assert_eq!(req.scan_type, fidl_mlme::ScanTypes::Passive);
        assert_eq!(req.ssid, b"foo".to_vec());
    }

    #[test]
    fn test_active_join_scan_args() {
        let mut sched = create_sched();
        let (_discarded_token, req) = sched.enqueue_scan_to_join(JoinScan {
            ssid: b"foo".to_vec(),
            token: 10,
            scan_type: fidl_common::ScanType::Active,
        });

        let req = req.expect("expected a ScanRequest");
        assert_eq!(req.scan_type, fidl_mlme::ScanTypes::Active);
        assert_eq!(req.ssid, b"foo".to_vec());
    }

    #[test]
    fn get_join_scan() {
        let mut sched = create_sched();
        assert_eq!(None, sched.get_join_scan());

        sched.enqueue_scan_to_join(passive_join_scan(b"foo".to_vec(), 10));
        // Make sure the scanner is in the state we expect it to be: the request is
        // 'current', not 'pending'
        assert_eq!(
            ScanState::ScanningToJoin {
                cmd: passive_join_scan(b"foo".to_vec(), 10),
                mlme_txn_id: 1,
                bss_list: vec![],
            },
            sched.current
        );
        assert_eq!(None, sched.pending_join);

        assert_eq!(Some(&passive_join_scan(b"foo".to_vec(), 10)), sched.get_join_scan());

        sched.enqueue_scan_to_join(passive_join_scan(b"bar".to_vec(), 20));
        // Again, make sure the state is what we expect. "Foo" should still be the current request,
        // while "bar" should be pending
        assert_eq!(
            ScanState::ScanningToJoin {
                cmd: passive_join_scan(b"foo".to_vec(), 10),
                mlme_txn_id: 1,
                bss_list: vec![],
            },
            sched.current
        );
        assert_eq!(Some(passive_join_scan(b"bar".to_vec(), 20)), sched.pending_join);

        // Expect the pending request to be returned since the current one will be discarded
        // once the scan finishes
        assert_eq!(Some(&passive_join_scan(b"bar".to_vec(), 20)), sched.get_join_scan());
    }

    fn create_sched() -> ScanScheduler<i32, i32> {
        ScanScheduler::new(Arc::new(DeviceInfo { addr: CLIENT_ADDR, bands: vec![] }))
    }
}
