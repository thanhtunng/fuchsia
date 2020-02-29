// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! This module is created in hope of unifying MLD and IGMPv2
//! and enable sharing the common state machine among these two
//! Group Management Protocols. The name GMP actually comes from
//! RFC 4604, quoted as saying: "Due to the commonality of function,
//! the term "Group Management Protocol", or "GMP", will be used to
//! refer to both IGMP and MLD.

use alloc::vec::{self, Vec};
use core::convert::TryFrom;
use core::time::Duration;

use rand::Rng;

use crate::Instant;

/// This trait is used to model the different parts of the two protocols.
///
/// Though MLD and IGMPv2 share the most part of their state machines
/// there are some subtle differences between each other.
pub(crate) trait ProtocolSpecific: Copy {
    /// The type for protocol-specific actions.
    type Action;
    /// The type for protocol-specific configs.
    type Config;

    /// The maximum delay to wait to send an unsolicited report.
    fn cfg_unsolicited_report_interval(cfg: &Self::Config) -> Duration;

    /// Whether the host should send a leave message even it is not the last host.
    fn cfg_send_leave_anyway(cfg: &Self::Config) -> bool;

    /// Get the _real_ `MAX_RESP_TIME`
    ///
    /// For IGMP messages, if the given duration is zero, it should be
    /// interpreted as 10 seconds; While for MLD messages, this function
    /// is the same as the identity function.
    fn get_max_resp_time(resp_time: Duration) -> Duration;

    /// Respond to a query in a protocol-specific way.
    ///
    /// When receiving a query, IGMPv2 needs to check whether the query
    /// is an IGMPv1 message and, if so, set a local "IGMPv1 Router Present"
    /// flag and set a timer. For MLD, this function is a no-op.
    fn do_query_received_specific(
        cfg: &Self::Config,
        actions: &mut Actions<Self>,
        max_resp_time: Duration,
        old: Self,
    ) -> Self;
}

/// This is used to represent the states that are common in both MLD and IGMPv2.
/// The state machine should behave as described on [RFC 2236 page 10] and
/// [RFC 2710 page 10].
///
/// [RFC 2236 page 10]: https://tools.ietf.org/html/rfc2236#page-10
/// [RFC 2710 page 10]: https://tools.ietf.org/html/rfc2710#page-10
pub(crate) struct GmpHostState<State, P: ProtocolSpecific> {
    state: State,
    /// `protocol_specific` are the value(s) you don't want the
    /// users to have a chance to modify. It is supposed to be
    /// only modified by the protocol itself.
    protocol_specific: P,
    /// `cfg` is used to store value(s) that is supposed to be
    /// modified by users.
    cfg: P::Config,
}

// Used to write tests in other modules
#[cfg(test)]
impl<S, P: ProtocolSpecific> GmpHostState<S, P> {
    pub(crate) fn get_protocol_specific(&self) -> P {
        self.protocol_specific
    }

    pub(crate) fn get_state(&self) -> &S {
        &self.state
    }
}

/// Generic actions that will be used both in MLD and IGMPv2.
///
/// The terms are biased towards IGMPv2. `Leave` is called `Done`
/// in MLD.
#[derive(Debug, PartialEq, Eq)]
pub(crate) enum GmpAction<P: ProtocolSpecific> {
    ScheduleReportTimer(Duration),
    StopReportTimer,
    SendLeave,
    SendReport(P),
}

/// The type to represent the actions generated by the state machine.
///
/// An action could either be a generic action as defined in `GmpAction`
/// Or any other protocol-specific action that is associated with `ProtocolSpecific`.
#[derive(Debug, PartialEq, Eq)]
pub(crate) enum Action<P: ProtocolSpecific> {
    Generic(GmpAction<P>),
    Specific(P::Action),
}

/// A collection of `Action`s.
// TODO: switch to ArrayVec if performance ever becomes a concern.
pub(crate) struct Actions<P: ProtocolSpecific>(Vec<Action<P>>);

impl<P: ProtocolSpecific> Actions<P> {
    /// Create an initially empty set of actions.
    pub(crate) fn nothing() -> Actions<P> {
        Actions(Vec::new())
    }

    /// Add a generic action to the set.
    pub(crate) fn push_generic(&mut self, action: GmpAction<P>) {
        self.0.push(Action::Generic(action));
    }

    /// Add a specific action to the set.
    pub(crate) fn push_specific(&mut self, action: P::Action) {
        self.0.push(Action::Specific(action));
    }
}

impl<P: ProtocolSpecific> IntoIterator for Actions<P> {
    type Item = Action<P>;
    type IntoIter = vec::IntoIter<Self::Item>;

    fn into_iter(self) -> Self::IntoIter {
        self.0.into_iter()
    }
}

/// The three states that are common in both MLD and IGMPv2.
///
/// Again, the terms used here are biased towards IGMPv2. In MLD,
/// their names are {Non, Delaying, Idle}-Listener instead.
pub(crate) enum MemberState<I: Instant, P: ProtocolSpecific> {
    NonMember(GmpHostState<NonMember, P>),
    Delaying(GmpHostState<DelayingMember<I>, P>),
    Idle(GmpHostState<IdleMember, P>),
}

/// The transition between one state and the next.
///
/// A `Transition` includes the next state to enter and any actions
/// to take while executing the transition.
struct Transition<S, P: ProtocolSpecific>(GmpHostState<S, P>, Actions<P>);

/// Represents Non Member-specific state variables. Empty for now.
pub(crate) struct NonMember;

/// Represents Delaying Member-specific state variables.
pub(crate) struct DelayingMember<I: Instant> {
    /// The expiration time for the current timer.
    /// Useful to check if the timer needs to be reset
    /// when a query arrives.
    pub(crate) timer_expiration: I,

    /// Used to indicate whether we need to send out a Leave message
    /// when we are leaving the group. This flag will become false
    /// once we heard about another reporter.
    pub(crate) last_reporter: bool,
}

/// Represents Idle Member-specific state variables.
pub(crate) struct IdleMember {
    /// Used to indicate whether we need to send out a Leave message
    /// when we are leaving the group.
    pub(crate) last_reporter: bool,
}

impl<S, P: ProtocolSpecific> GmpHostState<S, P> {
    /// Construct a `Transition` from this state into the new state `T` with
    /// the given actions.
    fn transition<T>(self, t: T, actions: Actions<P>) -> Transition<T, P> {
        Transition(
            GmpHostState { state: t, protocol_specific: self.protocol_specific, cfg: self.cfg },
            actions,
        )
    }

    /// Construct a `Transition` from this state into the new state `T` with
    /// a given protocol-specific value and actions.
    fn transition_with_protocol_specific<T>(
        self,
        t: T,
        ps: P,
        actions: Actions<P>,
    ) -> Transition<T, P> {
        Transition(GmpHostState { state: t, protocol_specific: ps, cfg: self.cfg }, actions)
    }
}

/// Compute the new timer expiration with given inputs.
///
/// # Arguments
///
/// * `old` is `None` if there are currently no timers, otherwise `Some(t)` where t is the old
///   instant when the currently installed timer should fire.
/// * `resp_time` is the maximum response time required by Query message.
/// * `now` is the current time.
/// * `ps` is the current protocol-specific state.
/// * `actions` is a mutable reference to the actions being built.
///
/// # Return value
///
/// The computed new expiration time.
fn compute_timer_expiration<I: Instant, P: ProtocolSpecific, R: Rng>(
    rng: &mut R,
    old: Option<I>,
    resp_time: Duration,
    now: I,
    ps: P,
    actions: &mut Actions<P>,
) -> I {
    let resp_time_deadline = P::get_max_resp_time(resp_time);
    if resp_time_deadline.as_millis() == 0 {
        actions.push_generic(GmpAction::SendReport(ps));
        now
    } else {
        let new_deadline = now.checked_add(resp_time_deadline).unwrap();
        let urgent = match old {
            Some(old) => new_deadline < old,
            None => true,
        };
        let delay = random_report_timeout(rng, resp_time_deadline);
        let timer_expiration = if urgent { now.checked_add(delay).unwrap() } else { old.unwrap() };
        if urgent {
            actions.push_generic(GmpAction::ScheduleReportTimer(delay));
        }
        timer_expiration
    }
}

/// Randomly generates a timeout in (0, period].
///
/// # Panics
///
/// `random_report_timeout` may panic if `period.as_micros()` overflows `u64`.
fn random_report_timeout<R: Rng>(rng: &mut R, period: Duration) -> Duration {
    let micros = rng.gen_range(0, u64::try_from(period.as_micros()).unwrap()) + 1;
    // u64 will be enough here because the only input of the function is from
    // the `MaxRespTime` field of the GMP query packets. The representable
    // number of microseconds is bounded by 2^33.
    Duration::from_micros(micros)
}

impl<P: ProtocolSpecific> GmpHostState<NonMember, P> {
    fn join_group<I: Instant, R: Rng>(
        self,
        rng: &mut R,
        now: I,
    ) -> Transition<DelayingMember<I>, P> {
        let duration = P::cfg_unsolicited_report_interval(&self.cfg);
        let delay = random_report_timeout(rng, duration);
        let mut actions = Actions::<P>::nothing();
        actions.push_generic(GmpAction::SendReport(self.protocol_specific));
        actions.push_generic(GmpAction::ScheduleReportTimer(delay));
        self.transition(
            DelayingMember {
                last_reporter: true,
                timer_expiration: now.checked_add(delay).expect("timer expiration overflowed"),
            },
            actions,
        )
    }
}

impl<I: Instant, P: ProtocolSpecific> GmpHostState<DelayingMember<I>, P> {
    fn query_received<R: Rng>(
        self,
        rng: &mut R,
        max_resp_time: Duration,
        now: I,
    ) -> Transition<DelayingMember<I>, P> {
        let mut actions = Actions::<P>::nothing();
        let last_reporter = self.state.last_reporter;
        let timer_expiration = compute_timer_expiration(
            rng,
            Some(self.state.timer_expiration),
            max_resp_time,
            now,
            self.protocol_specific,
            &mut actions,
        );
        let new_ps = P::do_query_received_specific(
            &self.cfg,
            &mut actions,
            max_resp_time,
            self.protocol_specific,
        );
        self.transition_with_protocol_specific(
            DelayingMember { last_reporter, timer_expiration },
            new_ps,
            actions,
        )
    }

    fn leave_group(self) -> Transition<NonMember, P> {
        let mut actions = Actions::<P>::nothing();
        if self.state.last_reporter || P::cfg_send_leave_anyway(&self.cfg) {
            actions.push_generic(GmpAction::SendLeave);
        }
        actions.push_generic(GmpAction::StopReportTimer);
        self.transition(NonMember {}, actions)
    }

    fn report_received(self) -> Transition<IdleMember, P> {
        let mut actions = Actions::<P>::nothing();
        actions.push_generic(GmpAction::StopReportTimer);
        self.transition(IdleMember { last_reporter: false }, actions)
    }

    fn report_timer_expired(self) -> Transition<IdleMember, P> {
        let mut actions = Actions::<P>::nothing();
        actions.push_generic(GmpAction::SendReport(self.protocol_specific));
        self.transition(IdleMember { last_reporter: true }, actions)
    }
}

impl<P: ProtocolSpecific> GmpHostState<IdleMember, P> {
    fn query_received<I: Instant, R: Rng>(
        self,
        rng: &mut R,
        max_resp_time: Duration,
        now: I,
    ) -> Transition<DelayingMember<I>, P> {
        let last_reporter = self.state.last_reporter;
        let mut actions = Actions::<P>::nothing();
        let timer_expiration = compute_timer_expiration(
            rng,
            None,
            max_resp_time,
            now,
            self.protocol_specific,
            &mut actions,
        );
        let new_ps = P::do_query_received_specific(
            &self.cfg,
            &mut actions,
            max_resp_time,
            self.protocol_specific,
        );
        self.transition_with_protocol_specific(
            DelayingMember { last_reporter, timer_expiration },
            new_ps,
            actions,
        )
    }

    fn leave_group(self) -> Transition<NonMember, P> {
        let mut actions = Actions::<P>::nothing();
        if self.state.last_reporter || P::cfg_send_leave_anyway(&self.cfg) {
            actions.push_generic(GmpAction::SendLeave);
        }
        self.transition(NonMember {}, actions)
    }
}

impl<I: Instant, P: ProtocolSpecific> From<GmpHostState<NonMember, P>> for MemberState<I, P> {
    fn from(s: GmpHostState<NonMember, P>) -> Self {
        MemberState::NonMember(s)
    }
}

impl<I: Instant, P: ProtocolSpecific> From<GmpHostState<DelayingMember<I>, P>>
    for MemberState<I, P>
{
    fn from(s: GmpHostState<DelayingMember<I>, P>) -> Self {
        MemberState::Delaying(s)
    }
}

impl<I: Instant, P: ProtocolSpecific> From<GmpHostState<IdleMember, P>> for MemberState<I, P> {
    fn from(s: GmpHostState<IdleMember, P>) -> Self {
        MemberState::Idle(s)
    }
}

impl<S, P: ProtocolSpecific> Transition<S, P> {
    fn into_state_actions<I: Instant>(self) -> (MemberState<I, P>, Actions<P>)
    where
        MemberState<I, P>: From<GmpHostState<S, P>>,
    {
        (self.0.into(), self.1)
    }
}

impl<I: Instant, P: ProtocolSpecific> MemberState<I, P> {
    fn join_group<R: Rng>(self, rng: &mut R, now: I) -> (MemberState<I, P>, Actions<P>) {
        match self {
            MemberState::NonMember(state) => state.join_group(rng, now).into_state_actions(),
            state => (state, Actions::nothing()),
        }
    }

    fn leave_group(self) -> (MemberState<I, P>, Actions<P>) {
        match self {
            MemberState::Delaying(state) => state.leave_group().into_state_actions(),
            MemberState::Idle(state) => state.leave_group().into_state_actions(),
            state => (state, Actions::nothing()),
        }
    }

    fn query_received<R: Rng>(
        self,
        rng: &mut R,
        max_resp_time: Duration,
        now: I,
    ) -> (MemberState<I, P>, Actions<P>) {
        match self {
            MemberState::Delaying(state) => {
                state.query_received(rng, max_resp_time, now).into_state_actions()
            }
            MemberState::Idle(state) => {
                state.query_received(rng, max_resp_time, now).into_state_actions()
            }
            state => (state, Actions::nothing()),
        }
    }

    fn report_received(self) -> (MemberState<I, P>, Actions<P>) {
        match self {
            MemberState::Delaying(state) => state.report_received().into_state_actions(),
            state => (state, Actions::nothing()),
        }
    }

    fn report_timer_expired(self) -> (MemberState<I, P>, Actions<P>) {
        match self {
            MemberState::Delaying(state) => state.report_timer_expired().into_state_actions(),
            state => (state, Actions::nothing()),
        }
    }
}

pub(crate) struct GmpStateMachine<I: Instant, P: ProtocolSpecific> {
    inner: Option<MemberState<I, P>>,
}

impl<I: Instant, P: ProtocolSpecific + Default> Default for GmpStateMachine<I, P>
where
    P::Config: Default,
{
    fn default() -> Self {
        GmpStateMachine {
            inner: Some(MemberState::NonMember(GmpHostState {
                protocol_specific: P::default(),
                cfg: P::Config::default(),
                state: NonMember {},
            })),
        }
    }
}

impl<I: Instant, P: ProtocolSpecific> GmpStateMachine<I, P> {
    /// When a "join group" command is received.
    pub(crate) fn join_group<R: Rng>(&mut self, rng: &mut R, now: I) -> Actions<P> {
        self.update(|s| s.join_group(rng, now))
    }

    /// When a "leave group" command is received.
    pub(crate) fn leave_group(&mut self) -> Actions<P> {
        self.update(MemberState::leave_group)
    }

    /// When a query is received, and we have to respond within max_resp_time.
    pub(crate) fn query_received<R: Rng>(
        &mut self,
        rng: &mut R,
        max_resp_time: Duration,
        now: I,
    ) -> Actions<P> {
        self.update(|s| s.query_received(rng, max_resp_time, now))
    }

    /// We have received a report from another host on our local network.
    pub(crate) fn report_received(&mut self) -> Actions<P> {
        self.update(MemberState::report_received)
    }

    /// The timer installed has expired.
    pub(crate) fn report_timer_expired(&mut self) -> Actions<P> {
        self.update(MemberState::report_timer_expired)
    }

    /// Update the state with no argument.
    pub(crate) fn update<F: FnOnce(MemberState<I, P>) -> (MemberState<I, P>, Actions<P>)>(
        &mut self,
        f: F,
    ) -> Actions<P> {
        let (s, a) = f(self.inner.take().unwrap());
        self.inner = Some(s);
        a
    }

    /// Update the state with a new protocol-specific value.
    pub(crate) fn update_with_protocol_specific(&mut self, ps: P) {
        self.update(|s| {
            (
                match s {
                    MemberState::NonMember(GmpHostState { state, cfg, .. }) => {
                        MemberState::NonMember(GmpHostState::<NonMember, P> {
                            state,
                            cfg,
                            protocol_specific: ps,
                        })
                    }
                    MemberState::Delaying(GmpHostState { state, cfg, .. }) => {
                        MemberState::Delaying(GmpHostState { state, cfg, protocol_specific: ps })
                    }
                    MemberState::Idle(GmpHostState { state, cfg, .. }) => {
                        MemberState::Idle(GmpHostState { state, cfg, protocol_specific: ps })
                    }
                },
                Actions::<P>::nothing(),
            )
        });
    }

    #[cfg(test)]
    pub(crate) fn get_inner(&self) -> &MemberState<I, P> {
        self.inner.as_ref().unwrap()
    }
}

#[cfg(test)]
mod test {
    use std::time::{Duration, Instant};

    use never::Never;

    use super::*;
    use crate::ip::gmp::{Action, GmpAction, MemberState};
    use crate::testutil::new_rng;

    const DEFAULT_UNSOLICITED_REPORT_INTERVAL: Duration = Duration::from_secs(10);

    /// Dummy `ProtocolSpecific` for test purposes.
    #[derive(PartialEq, Eq, Copy, Clone, Default)]
    struct DummyProtocolSpecific;

    impl ProtocolSpecific for DummyProtocolSpecific {
        /// Tests for generic state machine should not know anything about
        /// protocol specific actions.
        type Action = Never;

        /// Whether to send leave group message if our flag is not set.
        type Config = bool;

        fn cfg_unsolicited_report_interval(_cfg: &Self::Config) -> Duration {
            DEFAULT_UNSOLICITED_REPORT_INTERVAL
        }

        fn cfg_send_leave_anyway(cfg: &Self::Config) -> bool {
            *cfg
        }

        fn get_max_resp_time(resp_time: Duration) -> Duration {
            resp_time
        }

        fn do_query_received_specific(
            _cfg: &Self::Config,
            _actions: &mut Actions<Self>,
            _max_resp_time: Duration,
            old: Self,
        ) -> Self {
            old
        }
    }

    impl<P: ProtocolSpecific> GmpStateMachine<Instant, P> {
        pub(crate) fn get_config_mut(&mut self) -> &mut P::Config {
            match self.inner.as_mut().unwrap() {
                MemberState::NonMember(s) => &mut s.cfg,
                MemberState::Delaying(s) => &mut s.cfg,
                MemberState::Idle(s) => &mut s.cfg,
            }
        }
    }

    type DummyGmpStateMachine = GmpStateMachine<Instant, DummyProtocolSpecific>;

    fn at_least_one_action(
        actions: Actions<DummyProtocolSpecific>,
        action: Action<DummyProtocolSpecific>,
    ) -> bool {
        actions.into_iter().any(|a| a == action)
    }

    // whether there is at least one `SendReport` action within `upper` in the future
    fn at_least_one_report(actions: Actions<DummyProtocolSpecific>, upper: Duration) -> bool {
        actions.into_iter().any(|a| {
            if let Action::Generic(GmpAction::ScheduleReportTimer(d)) = a {
                d < upper
            } else {
                false
            }
        })
    }

    #[test]
    fn test_gmp_state_non_member_to_delay_should_set_flag() {
        let mut s = DummyGmpStateMachine::default();
        s.join_group(&mut new_rng(0), Instant::now());
        match s.get_inner() {
            MemberState::Delaying(s) => assert!(s.get_state().last_reporter),
            _ => panic!("Wrong State!"),
        }
    }

    #[test]
    fn test_gmp_state_non_member_to_delay_actions() {
        let mut s = DummyGmpStateMachine::default();
        let actions = s.join_group(&mut new_rng(0), Instant::now());
        assert!(at_least_one_action(
            actions,
            Action::<DummyProtocolSpecific>::Generic(GmpAction::SendReport(DummyProtocolSpecific,))
        ));
        let mut s = DummyGmpStateMachine::default();
        let actions = s.join_group(&mut new_rng(0), Instant::now());
        assert!(at_least_one_report(actions, DEFAULT_UNSOLICITED_REPORT_INTERVAL));
    }

    #[test]
    fn test_gmp_state_delay_dont_reset_timer() {
        let mut s = DummyGmpStateMachine::default();
        s.join_group(&mut new_rng(0), Instant::now());
        let actions = s.query_received(&mut new_rng(0), Duration::from_secs(100), Instant::now());
        for _ in actions {
            panic!("There should be no actions at all")
        }
    }
    #[test]
    fn test_gmp_state_delay_reset_timer() {
        let mut s = DummyGmpStateMachine::default();
        s.join_group(&mut new_rng(0), Instant::now());
        let actions = s.query_received(&mut new_rng(0), Duration::from_secs(1), Instant::now());
        at_least_one_report(actions, Duration::from_secs(1));
    }

    #[test]
    fn test_gmp_state_delay_to_idle_with_report_no_flag() {
        let mut s = DummyGmpStateMachine::default();
        s.join_group(&mut new_rng(0), Instant::now());
        s.report_received();
        match s.get_inner() {
            MemberState::Idle(s) => {
                assert!(!s.get_state().last_reporter);
            }
            _ => panic!("Wrong State!"),
        }
    }

    #[test]
    fn test_gmp_state_delay_to_idle_without_report_set_flag() {
        let mut s = DummyGmpStateMachine::default();
        s.join_group(&mut new_rng(0), Instant::now());
        s.report_timer_expired();
        match s.get_inner() {
            MemberState::Idle(s) => {
                assert!(s.get_state().last_reporter);
            }
            _ => panic!("Wrong State!"),
        }
    }

    #[test]
    fn test_gmp_state_leave_should_send_leave() {
        let mut s = DummyGmpStateMachine::default();
        s.join_group(&mut new_rng(0), Instant::now());
        let actions = s.leave_group();
        assert!(at_least_one_action(
            actions,
            Action::<DummyProtocolSpecific>::Generic(GmpAction::SendLeave)
        ));
        s.join_group(&mut new_rng(0), Instant::now());
        s.report_timer_expired();
        let actions = s.leave_group();
        assert!(at_least_one_action(
            actions,
            Action::<DummyProtocolSpecific>::Generic(GmpAction::SendLeave)
        ));
    }

    #[test]
    fn test_gmp_state_delay_to_other_states_should_stop_timer() {
        let mut s = DummyGmpStateMachine::default();
        s.join_group(&mut new_rng(0), Instant::now());
        let actions = s.leave_group();
        assert!(at_least_one_action(
            actions,
            Action::<DummyProtocolSpecific>::Generic(GmpAction::StopReportTimer)
        ));
        s.join_group(&mut new_rng(0), Instant::now());
        let actions = s.report_received();
        assert!(at_least_one_action(
            actions,
            Action::<DummyProtocolSpecific>::Generic(GmpAction::StopReportTimer)
        ));
    }

    #[test]
    fn test_gmp_state_other_states_to_delay_should_start_timer() {
        let mut s = DummyGmpStateMachine::default();
        let actions = s.join_group(&mut new_rng(0), Instant::now());
        assert!(at_least_one_report(actions, DEFAULT_UNSOLICITED_REPORT_INTERVAL));
        s.report_received();
        let actions = s.query_received(&mut new_rng(0), Duration::from_secs(1), Instant::now());
        assert!(at_least_one_report(actions, Duration::from_secs(1)));
    }

    #[test]
    fn test_gmp_state_leave_send_anyway_do_send() {
        let mut s = DummyGmpStateMachine::default();
        *s.get_config_mut() = true;
        s.join_group(&mut new_rng(0), Instant::now());
        s.report_received();
        match s.get_inner() {
            MemberState::Idle(s) => assert!(!s.get_state().last_reporter),
            _ => panic!("Wrong State!"),
        }
        let actions = s.leave_group();
        assert!(at_least_one_action(
            actions,
            Action::<DummyProtocolSpecific>::Generic(GmpAction::SendLeave)
        ));
    }

    #[test]
    fn test_gmp_state_leave_not_the_last_do_nothing() {
        let mut s = DummyGmpStateMachine::default();
        s.join_group(&mut new_rng(0), Instant::now());
        s.report_received();
        let actions = s.leave_group();
        for _ in actions {
            panic!("there should be no actions at all");
        }
    }
}
