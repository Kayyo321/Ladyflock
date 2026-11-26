/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Storage keys, https://storage.spec.whatwg.org/#storage-keys

#[cfg(not(test))]
use core::ffi::c_void;

#[cfg(not(test))]
use liburl_rust::FfiOrigin;
use liburl_rust::origin::Origin;

#[cfg(not(test))]
use crate::bindings::environment_settings_object::CppEnvironmentSettingsObject;

#[cfg(not(test))]
unsafe extern "C" {
    fn web_storage_api_obtain_a_storage_key(
        environment: *mut c_void,
        ctx: *mut c_void,
        on_origin: unsafe extern "C" fn(*mut c_void, *const FfiOrigin),
    ) -> bool;
    fn web_storage_api_obtain_a_storage_key_for_non_storage_purposes(
        environment: *mut c_void,
        ctx: *mut c_void,
        on_origin: unsafe extern "C" fn(*mut c_void, *const FfiOrigin),
    );
    fn web_storage_api_obtain_storage_partition_key(
        environment: *mut c_void,
        ctx: *mut c_void,
        on_partition: unsafe extern "C" fn(*mut c_void, *const u8, usize, bool),
    ) -> bool;
}

// https://storage.spec.whatwg.org/#storage-keys
// Partitioned by (origin, top_level_site, has_cross_site_ancestor) for web storage so same-origin
// frames with different top-level sites or cross-site ancestors (e.g. A -> B -> A) never share a mirror.
#[derive(Clone)]
pub(crate) struct StorageKey {
    pub(crate) origin: Origin,
    pub(crate) top_level_site: Option<String>,
    pub(crate) has_cross_site_ancestor: bool,
}

impl StorageKey {
    pub(crate) fn from_origin(origin: Origin) -> Self {
        Self {
            origin,
            top_level_site: None,
            has_cross_site_ancestor: false,
        }
    }
}

impl PartialEq for StorageKey {
    fn eq(&self, other: &Self) -> bool {
        self.origin.is_same_origin(&other.origin)
            && self.top_level_site == other.top_level_site
            && self.has_cross_site_ancestor == other.has_cross_site_ancestor
    }
}

#[cfg(not(test))]
unsafe extern "C" fn on_origin(ctx: *mut c_void, ffi_origin: *const FfiOrigin) {
    // SAFETY: ctx is the origin of the caller, and LibWeb lends the origin for the call.
    unsafe { *ctx.cast::<Option<Origin>>() = Some((*ffi_origin).to_origin()) };
}

#[cfg(not(test))]
unsafe extern "C" fn on_partition(
    ctx: *mut c_void,
    top_level_site_data: *const u8,
    top_level_site_len: usize,
    has_cross_site_ancestor: bool,
) {
    // SAFETY: LibWeb lends the UTF-8 byte slice for the duration of the callback.
    let bytes = if top_level_site_len == 0 {
        &[]
    } else {
        unsafe { core::slice::from_raw_parts(top_level_site_data, top_level_site_len) }
    };
    let site = String::from_utf8_lossy(bytes).into_owned();
    // SAFETY: ctx points to the caller's Option<(String, bool)>.
    unsafe { *ctx.cast::<Option<(String, bool)>>() = Some((site, has_cross_site_ancestor)) };
}

/// https://storage.spec.whatwg.org/#obtain-a-storage-key
#[cfg(not(test))]
pub(crate) fn obtain_a_storage_key(environment: &CppEnvironmentSettingsObject) -> Option<StorageKey> {
    let mut origin: Option<Origin> = None;
    // SAFETY: The environment settings object is live, and the origin outlives the call.
    unsafe { web_storage_api_obtain_a_storage_key(environment.as_ptr(), (&raw mut origin).cast(), on_origin) };
    let origin = origin?;

    let mut partition: Option<(String, bool)> = None;
    // SAFETY: The environment settings object is live, and partition outlives the call.
    let has_partition = unsafe {
        web_storage_api_obtain_storage_partition_key(environment.as_ptr(), (&raw mut partition).cast(), on_partition)
    };
    if !has_partition {
        return None;
    }
    let (top_level_site, has_cross_site_ancestor) = partition?;
    Some(StorageKey {
        origin,
        top_level_site: Some(top_level_site),
        has_cross_site_ancestor,
    })
}

/// https://storage.spec.whatwg.org/#obtain-a-storage-key-for-non-storage-purposes
#[cfg(not(test))]
pub(crate) fn obtain_a_storage_key_for_non_storage_purposes(environment: &CppEnvironmentSettingsObject) -> StorageKey {
    let mut origin: Option<Origin> = None;
    // SAFETY: The environment settings object is live, and the origin outlives the call.
    unsafe {
        web_storage_api_obtain_a_storage_key_for_non_storage_purposes(
            environment.as_ptr(),
            (&raw mut origin).cast(),
            on_origin,
        );
    }
    StorageKey::from_origin(origin.expect("LibWeb lends the origin of the storage key"))
}
