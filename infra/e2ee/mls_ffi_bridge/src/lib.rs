//! A narrow C API over OpenMLS (RFC 9420) for ULW's test and client harnesses.
//!
//! Clients and groups are opaque handles. Byte results come back in `UlwMlsBuffer`s the caller
//! releases with `ulw_mls_buffer_free`. Every entry point returns a `UlwMlsStatus` and catches
//! panics, so nothing unwinds into C. A handle is used by one thread at a time.
//!
//! The server never links this: it only moves the bytes these functions produce (ADR-0039).

use std::panic::{self, AssertUnwindSafe};
use std::ptr;
use std::slice;
use std::sync::{Arc, Once};

use openmls::prelude::tls_codec::Deserialize as _;
use openmls::prelude::*;
use openmls_basic_credential::SignatureKeyPair;
use openmls_rust_crypto::OpenMlsRustCrypto;

// The suite RFC 9420 makes mandatory to implement, so every MLS client can join our groups.
const CIPHERSUITE: Ciphersuite = Ciphersuite::MLS_128_DHKEMX25519_AES128GCM_SHA256_Ed25519;

// A member's identity is its device id; 64 bytes holds a UUID in any of its spellings.
const MAX_IDENTITY: usize = 64;

/// What a call did. Anything but `Ok` leaves every output empty.
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum UlwMlsStatus {
    Ok = 0,
    /// A null pointer, an empty or oversized identity or group id, or an empty batch.
    InvalidArgument = 1,
    /// The bytes do not decode as the MLS structure this call expects.
    Malformed = 2,
    /// The message decodes but the protocol refuses it: a bad signature, another group or
    /// epoch, a key package for another ciphersuite, a welcome for someone else.
    Rejected = 3,
    /// The member to remove is not in the group.
    NotAMember = 4,
    /// This member has been removed from the group; the group can no longer be used.
    Inactive = 5,
    /// Crypto or storage failed, or a bug panicked. Free the handles involved.
    Internal = 6,
}

/// What `ulw_mls_group_process` found in a message.
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum UlwMlsReceived {
    /// Plaintext is in the output buffer.
    Application = 0,
    /// Another member's commit, now merged: the group is at the next epoch.
    Commit = 1,
    /// A proposal, queued for the next commit.
    Proposal = 2,
}

/// Borrowed input bytes. `data` may be null when `len` is 0.
#[repr(C)]
pub struct UlwMlsBytes {
    pub data: *const u8,
    pub len: usize,
}

/// Bytes this library allocated. Release with `ulw_mls_buffer_free`.
#[repr(C)]
pub struct UlwMlsBuffer {
    pub data: *mut u8,
    pub len: usize,
}

struct Client {
    provider: OpenMlsRustCrypto,
    signer: SignatureKeyPair,
    credential: CredentialWithKey,
}

/// A device's MLS identity: its signature key, credential, and the private halves of the key
/// packages it has made.
pub struct UlwMlsClient {
    inner: Arc<Client>,
}

/// One device's view of one group. Shares its client, which lives until its last group does.
pub struct UlwMlsGroup {
    client: Arc<Client>,
    group: MlsGroup,
}

type Outcome = Result<(), UlwMlsStatus>;

// The default hook prints the panic, and OpenMLS values in it, to stderr of whatever process
// hosts the bridge. The status already reports it, so the message is dropped instead.
fn quiet_panics() {
    static QUIET: Once = Once::new();
    QUIET.call_once(|| panic::set_hook(Box::new(|_| {})));
}

// The group may be half-updated after a panic; the status tells the caller to drop it.
fn boundary(body: impl FnOnce() -> Outcome) -> UlwMlsStatus {
    quiet_panics();
    match panic::catch_unwind(AssertUnwindSafe(body)) {
        Ok(Ok(())) => UlwMlsStatus::Ok,
        Ok(Err(status)) => status,
        Err(_) => UlwMlsStatus::Internal,
    }
}

/// # Safety
/// `data` must be null or point at `len` readable bytes that stay put for the call.
unsafe fn input<'a>(data: *const u8, len: usize) -> Result<&'a [u8], UlwMlsStatus> {
    if len == 0 {
        return Ok(&[]);
    }
    if data.is_null() || len > isize::MAX as usize {
        return Err(UlwMlsStatus::InvalidArgument);
    }
    // SAFETY: non-null, and the caller vouches for `len` bytes.
    Ok(unsafe { slice::from_raw_parts(data, len) })
}

/// # Safety
/// `out` must be null or point at writable memory for one `UlwMlsBuffer`.
unsafe fn clear<'a>(out: *mut UlwMlsBuffer) -> Result<&'a mut UlwMlsBuffer, UlwMlsStatus> {
    // SAFETY: the caller vouches for `out` when it is not null.
    let out = unsafe { out.as_mut() }.ok_or(UlwMlsStatus::InvalidArgument)?;
    *out = UlwMlsBuffer {
        data: ptr::null_mut(),
        len: 0,
    };
    Ok(out)
}

fn fill(out: &mut UlwMlsBuffer, bytes: Vec<u8>) {
    let boxed = bytes.into_boxed_slice();
    let len = boxed.len();
    *out = UlwMlsBuffer {
        data: Box::into_raw(boxed).cast::<u8>(),
        len,
    };
}

fn serialize(message: &MlsMessageOut) -> Result<Vec<u8>, UlwMlsStatus> {
    message.to_bytes().map_err(|_| UlwMlsStatus::Internal)
}

fn decode(bytes: &[u8]) -> Result<MlsMessageBodyIn, UlwMlsStatus> {
    MlsMessageIn::tls_deserialize_exact(bytes)
        .map(MlsMessageIn::extract)
        .map_err(|_| UlwMlsStatus::Malformed)
}

fn state_error(error: &MlsGroupStateError) -> UlwMlsStatus {
    match error {
        MlsGroupStateError::UseAfterEviction => UlwMlsStatus::Inactive,
        MlsGroupStateError::LibraryError(_) => UlwMlsStatus::Internal,
        _ => UlwMlsStatus::Rejected,
    }
}

/// # Safety
/// `group` must be null or a live handle from this library, used by no other thread.
unsafe fn group_mut<'a>(group: *mut UlwMlsGroup) -> Result<&'a mut UlwMlsGroup, UlwMlsStatus> {
    // SAFETY: the caller vouches for the handle.
    unsafe { group.as_mut() }.ok_or(UlwMlsStatus::InvalidArgument)
}

/// # Safety
/// `client` must be null or a live handle from this library.
unsafe fn client_ref<'a>(client: *const UlwMlsClient) -> Result<&'a Arc<Client>, UlwMlsStatus> {
    // SAFETY: the caller vouches for the handle.
    unsafe { client.as_ref() }
        .map(|c| &c.inner)
        .ok_or(UlwMlsStatus::InvalidArgument)
}

/// Makes a client whose credential carries `identity`, 1 to 64 bytes.
///
/// # Safety
/// Pointers must be null or valid for the lengths given; `out` receives a handle to release
/// with `ulw_mls_client_free`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ulw_mls_client_new(
    identity: *const u8,
    identity_len: usize,
    out: *mut *mut UlwMlsClient,
) -> UlwMlsStatus {
    boundary(|| {
        // SAFETY: the caller vouches for `out` when it is not null.
        let out = unsafe { out.as_mut() }.ok_or(UlwMlsStatus::InvalidArgument)?;
        *out = ptr::null_mut();
        // SAFETY: as documented on this function.
        let identity = unsafe { input(identity, identity_len) }?;
        if identity.is_empty() || identity.len() > MAX_IDENTITY {
            return Err(UlwMlsStatus::InvalidArgument);
        }
        let signer = SignatureKeyPair::new(CIPHERSUITE.signature_algorithm())
            .map_err(|_| UlwMlsStatus::Internal)?;
        let credential = CredentialWithKey {
            credential: BasicCredential::new(identity.to_vec()).into(),
            signature_key: signer.to_public_vec().into(),
        };
        let client = UlwMlsClient {
            inner: Arc::new(Client {
                provider: OpenMlsRustCrypto::default(),
                signer,
                credential,
            }),
        };
        *out = Box::into_raw(Box::new(client));
        Ok(())
    })
}

/// # Safety
/// `client` must be null or a handle from `ulw_mls_client_new`, not used afterwards. Groups
/// made from it stay usable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ulw_mls_client_free(client: *mut UlwMlsClient) {
    if client.is_null() {
        return;
    }
    quiet_panics();
    let _ = panic::catch_unwind(AssertUnwindSafe(|| {
        // SAFETY: the caller hands the handle back exactly once.
        drop(unsafe { Box::from_raw(client) });
    }));
}

/// A fresh single-use KeyPackage, serialised as an MLSMessage, for the directory. Its private
/// init key stays in the client until a welcome consumes it.
///
/// # Safety
/// `client` must be a live handle and `out` writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ulw_mls_client_key_package(
    client: *const UlwMlsClient,
    out: *mut UlwMlsBuffer,
) -> UlwMlsStatus {
    boundary(|| {
        // SAFETY: as documented on this function.
        let out = unsafe { clear(out) }?;
        // SAFETY: as documented on this function.
        let client = unsafe { client_ref(client) }?;
        let bundle = KeyPackage::builder()
            .build(
                CIPHERSUITE,
                &client.provider,
                &client.signer,
                client.credential.clone(),
            )
            .map_err(|_| UlwMlsStatus::Internal)?;
        let message = MlsMessageOut::from(bundle.key_package().clone());
        fill(out, serialize(&message)?);
        Ok(())
    })
}

/// Starts a group, at epoch 0, with the client as its only member.
///
/// # Safety
/// `client` must be a live handle, `group_id` valid for `group_id_len` bytes, and `out`
/// writable; it receives a handle to release with `ulw_mls_group_free`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ulw_mls_group_create(
    client: *const UlwMlsClient,
    group_id: *const u8,
    group_id_len: usize,
    out: *mut *mut UlwMlsGroup,
) -> UlwMlsStatus {
    boundary(|| {
        // SAFETY: as documented on this function.
        let out = unsafe { out.as_mut() }.ok_or(UlwMlsStatus::InvalidArgument)?;
        *out = ptr::null_mut();
        // SAFETY: as documented on this function.
        let client = unsafe { client_ref(client) }?;
        // SAFETY: as documented on this function.
        let group_id = unsafe { input(group_id, group_id_len) }?;
        if group_id.is_empty() || group_id.len() > MAX_IDENTITY {
            return Err(UlwMlsStatus::InvalidArgument);
        }
        // The welcome carries the ratchet tree, so joining needs no second message.
        let config = MlsGroupCreateConfig::builder()
            .ciphersuite(CIPHERSUITE)
            .use_ratchet_tree_extension(true)
            .build();
        let group = MlsGroup::new_with_group_id(
            &client.provider,
            &client.signer,
            &config,
            GroupId::from_slice(group_id),
            client.credential.clone(),
        )
        .map_err(|e| match e {
            NewGroupError::GroupAlreadyExists => UlwMlsStatus::Rejected,
            _ => UlwMlsStatus::Internal,
        })?;
        *out = Box::into_raw(Box::new(UlwMlsGroup {
            client: Arc::clone(client),
            group,
        }));
        Ok(())
    })
}

/// Joins the group a welcome invites the client into, using the key package it names.
///
/// # Safety
/// As for `ulw_mls_group_create`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ulw_mls_group_join(
    client: *const UlwMlsClient,
    welcome: *const u8,
    welcome_len: usize,
    out: *mut *mut UlwMlsGroup,
) -> UlwMlsStatus {
    boundary(|| {
        // SAFETY: as documented on this function.
        let out = unsafe { out.as_mut() }.ok_or(UlwMlsStatus::InvalidArgument)?;
        *out = ptr::null_mut();
        // SAFETY: as documented on this function.
        let client = unsafe { client_ref(client) }?;
        // SAFETY: as documented on this function.
        let bytes = unsafe { input(welcome, welcome_len) }?;
        let MlsMessageBodyIn::Welcome(welcome) = decode(bytes)? else {
            return Err(UlwMlsStatus::Malformed);
        };
        let config = MlsGroupJoinConfig::builder()
            .use_ratchet_tree_extension(true)
            .build();
        let staged = StagedWelcome::new_from_welcome(&client.provider, &config, welcome, None)
            .map_err(|e| match e {
                WelcomeError::LibraryError(_) | WelcomeError::StorageError(_) => {
                    UlwMlsStatus::Internal
                }
                _ => UlwMlsStatus::Rejected,
            })?;
        let group = staged
            .into_group(&client.provider)
            .map_err(|_| UlwMlsStatus::Internal)?;
        *out = Box::into_raw(Box::new(UlwMlsGroup {
            client: Arc::clone(client),
            group,
        }));
        Ok(())
    })
}

/// # Safety
/// `group` must be null or a handle from this library, not used afterwards.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ulw_mls_group_free(group: *mut UlwMlsGroup) {
    if group.is_null() {
        return;
    }
    quiet_panics();
    let _ = panic::catch_unwind(AssertUnwindSafe(|| {
        // SAFETY: the caller hands the handle back exactly once.
        let mut handle = unsafe { Box::from_raw(group) };
        // The group's secrets live in the client's store, which may outlive this handle.
        let client = Arc::clone(&handle.client);
        let _ = handle.group.delete(client.provider.storage());
    }));
}

/// Commits adding the members whose key packages are given. The commit stays pending: send
/// `commit_out` to the group and `welcome_out` to the new members only once the delivery
/// service has accepted the commit for this epoch, then call
/// `ulw_mls_group_merge_pending_commit`; if it was refused, call
/// `ulw_mls_group_clear_pending_commit`.
///
/// # Safety
/// `group` must be a live handle, `key_packages` valid for `count` entries, each valid for its
/// length, and both outputs writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ulw_mls_group_add(
    group: *mut UlwMlsGroup,
    key_packages: *const UlwMlsBytes,
    count: usize,
    commit_out: *mut UlwMlsBuffer,
    welcome_out: *mut UlwMlsBuffer,
) -> UlwMlsStatus {
    boundary(|| {
        // SAFETY: as documented on this function.
        let commit_out = unsafe { clear(commit_out) }?;
        // SAFETY: as documented on this function.
        let welcome_out = unsafe { clear(welcome_out) }?;
        // SAFETY: as documented on this function.
        let handle = unsafe { group_mut(group) }?;
        if count == 0 || key_packages.is_null() || count > isize::MAX as usize {
            return Err(UlwMlsStatus::InvalidArgument);
        }
        // SAFETY: non-null, and the caller vouches for `count` entries.
        let entries = unsafe { slice::from_raw_parts(key_packages, count) };
        let client = Arc::clone(&handle.client);
        let mut packages = Vec::with_capacity(count);
        for entry in entries {
            // SAFETY: as documented on this function.
            let bytes = unsafe { input(entry.data, entry.len) }?;
            let MlsMessageBodyIn::KeyPackage(package) = decode(bytes)? else {
                return Err(UlwMlsStatus::Malformed);
            };
            let package = package
                .validate(client.provider.crypto(), ProtocolVersion::Mls10)
                .map_err(|_| UlwMlsStatus::Rejected)?;
            packages.push(package);
        }
        let (commit, welcome, _) = handle
            .group
            .add_members(&client.provider, &client.signer, &packages)
            .map_err(|e| match e {
                AddMembersError::GroupStateError(state) => state_error(&state),
                AddMembersError::LibraryError(_) | AddMembersError::StorageError(_) => {
                    UlwMlsStatus::Internal
                }
                _ => UlwMlsStatus::Rejected,
            })?;
        let commit = serialize(&commit)?;
        let welcome = serialize(&welcome)?;
        fill(commit_out, commit);
        fill(welcome_out, welcome);
        Ok(())
    })
}

/// Commits removing the member whose credential carries `identity`. Pending, as for
/// `ulw_mls_group_add`.
///
/// # Safety
/// `group` must be a live handle, `identity` valid for its length and `commit_out` writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ulw_mls_group_remove(
    group: *mut UlwMlsGroup,
    identity: *const u8,
    identity_len: usize,
    commit_out: *mut UlwMlsBuffer,
) -> UlwMlsStatus {
    boundary(|| {
        // SAFETY: as documented on this function.
        let commit_out = unsafe { clear(commit_out) }?;
        // SAFETY: as documented on this function.
        let handle = unsafe { group_mut(group) }?;
        // SAFETY: as documented on this function.
        let identity = unsafe { input(identity, identity_len) }?;
        if identity.is_empty() {
            return Err(UlwMlsStatus::InvalidArgument);
        }
        let member = handle
            .group
            .members()
            .find(|m| m.credential.serialized_content() == identity)
            .ok_or(UlwMlsStatus::NotAMember)?;
        let client = Arc::clone(&handle.client);
        let (commit, _, _) = handle
            .group
            .remove_members(&client.provider, &client.signer, &[member.index])
            .map_err(|e| match e {
                RemoveMembersError::GroupStateError(state) => state_error(&state),
                RemoveMembersError::LibraryError(_) | RemoveMembersError::StorageError(_) => {
                    UlwMlsStatus::Internal
                }
                _ => UlwMlsStatus::Rejected,
            })?;
        fill(commit_out, serialize(&commit)?);
        Ok(())
    })
}

/// Applies this member's pending commit: the delivery service accepted it.
///
/// # Safety
/// `group` must be a live handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ulw_mls_group_merge_pending_commit(
    group: *mut UlwMlsGroup,
) -> UlwMlsStatus {
    boundary(|| {
        // SAFETY: as documented on this function.
        let handle = unsafe { group_mut(group) }?;
        if handle.group.pending_commit().is_none() {
            return Err(UlwMlsStatus::Rejected);
        }
        let client = Arc::clone(&handle.client);
        handle
            .group
            .merge_pending_commit(&client.provider)
            .map_err(|e| match e {
                MergePendingCommitError::MlsGroupStateError(state) => state_error(&state),
                _ => UlwMlsStatus::Internal,
            })
    })
}

/// Drops this member's pending commit: another commit won the epoch.
///
/// # Safety
/// `group` must be a live handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ulw_mls_group_clear_pending_commit(
    group: *mut UlwMlsGroup,
) -> UlwMlsStatus {
    boundary(|| {
        // SAFETY: as documented on this function.
        let handle = unsafe { group_mut(group) }?;
        let client = Arc::clone(&handle.client);
        handle
            .group
            .clear_pending_commit(client.provider.storage())
            .map_err(|_| UlwMlsStatus::Internal)
    })
}

/// Encrypts an application message for the group's current epoch.
///
/// # Safety
/// `group` must be a live handle, `plaintext` valid for its length and `out` writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ulw_mls_group_encrypt(
    group: *mut UlwMlsGroup,
    plaintext: *const u8,
    plaintext_len: usize,
    out: *mut UlwMlsBuffer,
) -> UlwMlsStatus {
    boundary(|| {
        // SAFETY: as documented on this function.
        let out = unsafe { clear(out) }?;
        // SAFETY: as documented on this function.
        let handle = unsafe { group_mut(group) }?;
        // SAFETY: as documented on this function.
        let plaintext = unsafe { input(plaintext, plaintext_len) }?;
        let client = Arc::clone(&handle.client);
        let message = handle
            .group
            .create_message(&client.provider, &client.signer, plaintext)
            .map_err(|e| match e {
                CreateMessageError::GroupStateError(state) => state_error(&state),
                CreateMessageError::LibraryError(_) => UlwMlsStatus::Internal,
            })?;
        fill(out, serialize(&message)?);
        Ok(())
    })
}

/// Processes a message another member sent to the group. Application messages decrypt into
/// `plaintext_out`; commits are merged, since the delivery service has already ordered them;
/// proposals are queued. A member never processes its own messages.
///
/// # Safety
/// `group` must be a live handle, `message` valid for its length, and both outputs writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ulw_mls_group_process(
    group: *mut UlwMlsGroup,
    message: *const u8,
    message_len: usize,
    received: *mut UlwMlsReceived,
    plaintext_out: *mut UlwMlsBuffer,
) -> UlwMlsStatus {
    boundary(|| {
        // SAFETY: as documented on this function.
        let plaintext_out = unsafe { clear(plaintext_out) }?;
        // SAFETY: the caller vouches for `received` when it is not null.
        let received = unsafe { received.as_mut() }.ok_or(UlwMlsStatus::InvalidArgument)?;
        // SAFETY: as documented on this function.
        let handle = unsafe { group_mut(group) }?;
        // SAFETY: as documented on this function.
        let bytes = unsafe { input(message, message_len) }?;
        let protocol_message = MlsMessageIn::tls_deserialize_exact(bytes)
            .map_err(|_| UlwMlsStatus::Malformed)?
            .try_into_protocol_message()
            .map_err(|_| UlwMlsStatus::Malformed)?;
        let client = Arc::clone(&handle.client);
        let processed = handle
            .group
            .process_message(&client.provider, protocol_message)
            .map_err(|e| match e {
                ProcessMessageError::GroupStateError(state) => state_error(&state),
                ProcessMessageError::LibraryError(_) | ProcessMessageError::StorageError(_) => {
                    UlwMlsStatus::Internal
                }
                _ => UlwMlsStatus::Rejected,
            })?;
        match processed.into_content() {
            ProcessedMessageContent::ApplicationMessage(application) => {
                fill(plaintext_out, application.into_bytes());
                *received = UlwMlsReceived::Application;
            }
            ProcessedMessageContent::ProposalMessage(proposal) => {
                handle
                    .group
                    .store_pending_proposal(client.provider.storage(), *proposal)
                    .map_err(|_| UlwMlsStatus::Internal)?;
                *received = UlwMlsReceived::Proposal;
            }
            ProcessedMessageContent::StagedCommitMessage(commit) => {
                handle
                    .group
                    .merge_staged_commit(&client.provider, *commit)
                    .map_err(|_| UlwMlsStatus::Internal)?;
                *received = UlwMlsReceived::Commit;
            }
            // Nothing outside the group may propose, and a member's own messages come back
            // only if the room echoes them; it merged its commit when the service accepted it.
            ProcessedMessageContent::ExternalJoinProposalMessage(_)
            | ProcessedMessageContent::OwnPendingCommit
            | ProcessedMessageContent::OwnPrivateMessage => {
                return Err(UlwMlsStatus::Rejected);
            }
        }
        Ok(())
    })
}

/// The group's current epoch, which the delivery service orders commits by.
///
/// # Safety
/// `group` must be a live handle and `out` writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ulw_mls_group_epoch(
    group: *const UlwMlsGroup,
    out: *mut u64,
) -> UlwMlsStatus {
    boundary(|| {
        // SAFETY: as documented on this function.
        let out = unsafe { out.as_mut() }.ok_or(UlwMlsStatus::InvalidArgument)?;
        // SAFETY: as documented on this function.
        let handle = unsafe { group.as_ref() }.ok_or(UlwMlsStatus::InvalidArgument)?;
        *out = handle.group.epoch().as_u64();
        Ok(())
    })
}

/// How many members the group has as this member sees it.
///
/// # Safety
/// `group` must be a live handle and `out` writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ulw_mls_group_member_count(
    group: *const UlwMlsGroup,
    out: *mut usize,
) -> UlwMlsStatus {
    boundary(|| {
        // SAFETY: as documented on this function.
        let out = unsafe { out.as_mut() }.ok_or(UlwMlsStatus::InvalidArgument)?;
        // SAFETY: as documented on this function.
        let handle = unsafe { group.as_ref() }.ok_or(UlwMlsStatus::InvalidArgument)?;
        *out = handle.group.members().count();
        Ok(())
    })
}

/// # Safety
/// `buffer` must be empty or have come from this library, and be released once.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ulw_mls_buffer_free(buffer: UlwMlsBuffer) {
    if buffer.data.is_null() {
        return;
    }
    // SAFETY: `fill` made this pointer and length from a boxed slice.
    drop(unsafe { Box::from_raw(ptr::slice_from_raw_parts_mut(buffer.data, buffer.len)) });
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_panic_becomes_a_status() {
        let status = boundary(|| panic!("bug"));
        assert_eq!(status, UlwMlsStatus::Internal);
    }

    #[test]
    fn outputs_are_cleared_when_a_call_fails() {
        let mut buffer = UlwMlsBuffer {
            data: ptr::dangling_mut(),
            len: 7,
        };
        // SAFETY: a null handle is part of the contract under test.
        let status = unsafe { ulw_mls_client_key_package(ptr::null(), &mut buffer) };
        assert_eq!(status, UlwMlsStatus::InvalidArgument);
        assert!(buffer.data.is_null());
        assert_eq!(buffer.len, 0);
    }

    // The header is generated from this file; this fails when the two drift apart.
    // ULW_BLESS_HEADER=1 rewrites it instead.
    #[test]
    fn committed_header_matches_the_exported_api() {
        let dir = env!("CARGO_MANIFEST_DIR");
        let config = cbindgen::Config::from_file(format!("{dir}/cbindgen.toml")).unwrap();
        let bindings = cbindgen::Builder::new()
            .with_config(config)
            .with_src(format!("{dir}/src/lib.rs"))
            .generate()
            .unwrap();
        let mut generated = Vec::new();
        bindings.write(&mut generated);
        let path = format!("{dir}/include/ulw/mls_ffi_bridge.h");
        if std::env::var_os("ULW_BLESS_HEADER").is_some() {
            std::fs::write(&path, &generated).unwrap();
        }
        let committed = std::fs::read(&path).unwrap_or_default();
        assert!(
            committed == generated,
            "{path} is stale; rerun with ULW_BLESS_HEADER=1"
        );
    }
}
