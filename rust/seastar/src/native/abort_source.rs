/*
 * Copyright (C) 2023-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

use std::cell::Cell;
use std::fmt::{Display, Formatter};
use std::future::Future;

use super::condition_variable::ConditionVariable;

pub struct AbortSource {
    cvar: ConditionVariable,
    aborted: Cell<bool>,
}

impl AbortSource {
    #[inline]
    pub fn new() -> Self {
        Self {
            cvar: ConditionVariable::new(),
            aborted: Cell::new(false),
        }
    }

    #[inline]
    pub fn new_aborted() -> Self {
        Self {
            cvar: ConditionVariable::new(),
            aborted: Cell::new(true),
        }
    }

    #[inline]
    pub fn is_aborted(&self) -> bool {
        self.aborted.get()
    }

    #[inline]
    pub fn check(&self) -> Result<(), AbortRequestedError> {
        if !self.aborted.get() {
            Ok(())
        } else {
            Err(AbortRequestedError)
        }
    }

    #[inline]
    pub fn wait_until_aborted(&self) -> impl Future<Output = Result<(), AbortRequestedError>> + '_ {
        async {
            self.cvar.wait_until(|| self.aborted.get()).await;
            Err(AbortRequestedError)
        }
    }

    #[inline]
    pub fn request_abort(&self) {
        self.aborted.set(true);
        self.cvar.broadcast();
    }
}

#[derive(Debug)]
pub struct AbortRequestedError;

impl Display for AbortRequestedError {
    #[inline]
    fn fmt(&self, f: &mut Formatter<'_>) -> std::fmt::Result {
        f.write_str("abort requested")
    }
}
