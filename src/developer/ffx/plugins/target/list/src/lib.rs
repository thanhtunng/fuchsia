// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::target_formatter::TargetFormatter,
    anyhow::{anyhow, Result},
    errors::ffx_bail_with_code,
    ffx_core::ffx_plugin,
    ffx_list_args::ListCommand,
    fidl_fuchsia_developer_bridge::{TargetCollectionIteratorMarker, TargetCollectionProxy},
    std::convert::TryFrom,
    std::io::{stdout, Write},
};

mod target_formatter;

#[ffx_plugin(TargetCollectionProxy = "daemon::service")]
pub async fn list_targets(tc_proxy: TargetCollectionProxy, cmd: ListCommand) -> Result<()> {
    list_impl(tc_proxy, cmd, &mut stdout()).await
}

async fn list_impl<W: Write>(
    tc_proxy: TargetCollectionProxy,
    cmd: ListCommand,
    writer: &mut W,
) -> Result<()> {
    let (iterator_proxy, server) =
        fidl::endpoints::create_proxy::<TargetCollectionIteratorMarker>()?;
    tc_proxy.list_targets(cmd.nodename.as_ref().map(|s| s.as_str()), server).await?;
    let mut res = Vec::new();
    loop {
        let r = iterator_proxy
            .get_next()
            .await
            .map_err(|e| anyhow!("Error listing targets: {:?}", e))?;
        if r.len() > 0 {
            res.extend(r);
        } else {
            break;
        }
    }

    match res.len() {
        0 => {
            // Printed to stderr, so that if a user is parsing output, say from a formatted
            // output, that the message is not consumed. A stronger future strategy would
            // have richer behavior dependent upon whether the user has a controlling
            // terminal, which would require passing in more and richer IO delegates.
            if let Some(n) = cmd.nodename {
                ffx_bail_with_code!(2, "Device {} not found.", n);
            } else {
                eprintln!("No devices found.");
            }
        }
        _ => {
            let formatter = Box::<dyn TargetFormatter>::try_from((cmd.format, res))?;
            let default: Option<String> = ffx_config::get("target.default").await?;
            writeln!(writer, "{}", formatter.lines(default.as_deref()).join("\n"))?;
        }
    };
    Ok(())
}

///////////////////////////////////////////////////////////////////////////////
// tests

#[cfg(test)]
mod test {
    use {
        super::*,
        addr::TargetAddr,
        ffx_list_args::Format,
        fidl_fuchsia_developer_bridge as bridge,
        fidl_fuchsia_developer_bridge::{
            RemoteControlState, Target as FidlTarget, TargetState, TargetType,
        },
        futures_lite::stream::StreamExt,
        regex::Regex,
        std::net::IpAddr,
    };

    fn tab_list_cmd(nodename: Option<String>) -> ListCommand {
        ListCommand { nodename, format: Format::Tabular }
    }

    fn to_fidl_target(nodename: String) -> FidlTarget {
        let addr: TargetAddr =
            (IpAddr::from([0xfe80, 0x0, 0x0, 0x0, 0xdead, 0xbeef, 0xbeef, 0xbeef]), 3).into();
        FidlTarget {
            nodename: Some(nodename),
            addresses: Some(vec![addr.into()]),
            age_ms: Some(101),
            rcs_state: Some(RemoteControlState::Up),
            target_type: Some(TargetType::Unknown),
            target_state: Some(TargetState::Unknown),
            ..FidlTarget::EMPTY
        }
    }

    fn setup_fake_target_collection_server(num_tests: usize) -> TargetCollectionProxy {
        setup_fake_tc_proxy(move |req| match req {
            bridge::TargetCollectionRequest::ListTargets { query, iterator, responder } => {
                let fidl_values: Vec<FidlTarget> =
                    if query.as_ref().map(|s| s.is_empty()).unwrap_or(true) {
                        (0..num_tests)
                            .map(|i| format!("Test {}", i))
                            .map(|name| to_fidl_target(name))
                            .collect()
                    } else {
                        let v = query.unwrap();
                        (0..num_tests)
                            .map(|i| format!("Test {}", i))
                            .filter(|t| *t == v)
                            .map(|name| to_fidl_target(name))
                            .collect()
                    };
                let mut stream = iterator.into_stream().unwrap();
                fuchsia_async::Task::local(async move {
                    const CHUNK_SIZE: usize = 10;
                    let mut iter = fidl_values.into_iter();
                    while let Ok(Some(bridge::TargetCollectionIteratorRequest::GetNext {
                        responder,
                    })) = stream.try_next().await
                    {
                        responder
                            .send(
                                &mut iter.by_ref().take(CHUNK_SIZE).collect::<Vec<_>>().into_iter(),
                            )
                            .unwrap();
                    }
                })
                .detach();
                responder.send().unwrap();
            }
        })
    }

    async fn try_run_list_test(num_tests: usize, cmd: ListCommand) -> Result<String> {
        let mut writer = Vec::new();
        let proxy = setup_fake_target_collection_server(num_tests);
        list_impl(proxy, cmd, &mut writer).await.map(|_| String::from_utf8(writer).unwrap())
    }

    async fn run_list_test(num_tests: usize, cmd: ListCommand) -> String {
        try_run_list_test(num_tests, cmd).await.unwrap()
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_list_with_no_devices_and_no_nodename() -> Result<()> {
        let output = run_list_test(0, tab_list_cmd(None)).await;
        assert_eq!("".to_string(), output);
        Ok(())
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_list_with_one_device_and_no_nodename() -> Result<()> {
        let output = run_list_test(1, tab_list_cmd(None)).await;
        let value = format!("Test {}", 0);
        let node_listing = Regex::new(&value).expect("test regex");
        assert_eq!(
            1,
            node_listing.find_iter(&output).count(),
            "could not find \"{}\" nodename in output:\n{}",
            value,
            output
        );
        Ok(())
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_list_with_multiple_devices_and_no_nodename() -> Result<()> {
        let num_tests = 10;
        let output = run_list_test(num_tests, tab_list_cmd(None)).await;
        for x in 0..num_tests {
            let value = format!("Test {}", x);
            let node_listing = Regex::new(&value).expect("test regex");
            assert_eq!(
                1,
                node_listing.find_iter(&output).count(),
                "could not find \"{}\" nodename in output:\n{}",
                value,
                output
            );
        }
        Ok(())
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_list_with_one_device_and_matching_nodename() -> Result<()> {
        let output = run_list_test(1, tab_list_cmd(Some("Test 0".to_string()))).await;
        let value = format!("Test {}", 0);
        let node_listing = Regex::new(&value).expect("test regex");
        assert_eq!(
            1,
            node_listing.find_iter(&output).count(),
            "could not find \"{}\" nodename in output:\n{}",
            value,
            output
        );
        Ok(())
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_list_with_one_device_and_not_matching_nodename() -> Result<()> {
        let output = try_run_list_test(1, tab_list_cmd(Some("blarg".to_string()))).await;
        assert!(output.is_err());
        Ok(())
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_list_with_multiple_devices_and_not_matching_nodename() -> Result<()> {
        let num_tests = 25;
        let output = try_run_list_test(num_tests, tab_list_cmd(Some("blarg".to_string()))).await;
        assert!(output.is_err());
        Ok(())
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_list_with_multiple_devices_and_matching_nodename() -> Result<()> {
        let output = run_list_test(25, tab_list_cmd(Some("Test 19".to_string()))).await;
        let value = format!("Test {}", 0);
        let node_listing = Regex::new(&value).expect("test regex");
        assert_eq!(0, node_listing.find_iter(&output).count());
        let value = format!("Test {}", 19);
        let node_listing = Regex::new(&value).expect("test regex");
        assert_eq!(1, node_listing.find_iter(&output).count());
        Ok(())
    }
}
