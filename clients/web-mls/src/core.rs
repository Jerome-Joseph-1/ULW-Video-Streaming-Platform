//! The client, in plain Rust: what `wasm.rs` exposes to JavaScript, and what the native tests
//! drive. Every choice that reaches the wire is the FFI bridge's (infra/e2ee/mls_ffi_bridge,
//! ADR-0044), so the two interoperate: the ciphersuite, basic credentials carrying the device's
//! identity, the ratchet tree in the welcome, OpenMLS's default wire format policy, and every
//! message serialised as an MLSMessage (RFC 9420, section 6).

use std::collections::HashMap;

use openmls::prelude::tls_codec::Deserialize as _;
use openmls::prelude::*;
use openmls_basic_credential::SignatureKeyPair;
use openmls_rust_crypto::OpenMlsRustCrypto;
use openmls_traits::OpenMlsProvider;

/// The suite RFC 9420 makes mandatory, and the only one the bridge speaks.
pub const CIPHERSUITE: Ciphersuite = Ciphersuite::MLS_128_DHKEMX25519_AES128GCM_SHA256_Ed25519;

/// A device id or a group id: 1 to 64 bytes, as the bridge allows.
pub const MAX_IDENTITY: usize = 64;

// Exported state starts with this, then a format version.
const STATE_MAGIC: &[u8; 8] = b"ULWMLS\0\x01";

/// The bridge's `UlwMlsStatus`, less `Ok`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Error {
    /// An empty or oversized identity or group id, or an empty batch.
    InvalidArgument,
    /// The bytes do not decode as the MLS structure this call expects.
    Malformed,
    /// The message decodes but the protocol refuses it: a bad signature, another group or
    /// epoch, a key package for another ciphersuite, a welcome for someone else.
    Rejected,
    /// The member to remove is not in the group.
    NotAMember,
    /// This member has been removed from the group.
    Inactive,
    /// Crypto or storage failed.
    Internal,
}

impl Error {
    /// The name JavaScript sees as the error's message.
    pub fn as_str(self) -> &'static str {
        match self {
            Error::InvalidArgument => "invalid_argument",
            Error::Malformed => "malformed",
            Error::Rejected => "rejected",
            Error::NotAMember => "not_a_member",
            Error::Inactive => "inactive",
            Error::Internal => "internal",
        }
    }
}

pub type Result<T> = std::result::Result<T, Error>;

/// What `process` found in a message.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Kind {
    Application,
    Commit,
    Proposal,
}

#[derive(Debug)]
pub struct Received {
    pub kind: Kind,
    /// The plaintext of an application message; empty otherwise.
    pub plaintext: Vec<u8>,
    /// The identity in the sender's credential.
    pub sender: Vec<u8>,
}

/// What an MLSMessage is, read without any keys: how a room's bodies are told apart.
#[derive(Debug, PartialEq, Eq)]
pub struct Info {
    /// `public_message`, `private_message`, `welcome`, `group_info` or `key_package`.
    pub wire_format: &'static str,
    /// For a public or private message: its group and epoch.
    pub group_id: Option<Vec<u8>>,
    pub epoch: Option<u64>,
    /// For a public or private message: `application`, `proposal` or `commit`.
    pub content_type: Option<&'static str>,
    /// For a key package: the identity in its credential, once its signature checks.
    pub identity: Option<Vec<u8>>,
}

/// A device's MLS identity: its signature key, its credential, and a store holding the private
/// halves of its key packages and every group's state.
pub struct Client {
    provider: OpenMlsRustCrypto,
    signer: SignatureKeyPair,
    credential: CredentialWithKey,
    identity: Vec<u8>,
}

fn check_id(id: &[u8]) -> Result<()> {
    if id.is_empty() || id.len() > MAX_IDENTITY {
        return Err(Error::InvalidArgument);
    }
    Ok(())
}

fn serialize(message: &MlsMessageOut) -> Result<Vec<u8>> {
    message.to_bytes().map_err(|_| Error::Internal)
}

fn decode(bytes: &[u8]) -> Result<MlsMessageBodyIn> {
    MlsMessageIn::tls_deserialize_exact(bytes)
        .map(MlsMessageIn::extract)
        .map_err(|_| Error::Malformed)
}

fn state_error(error: &MlsGroupStateError) -> Error {
    match error {
        MlsGroupStateError::UseAfterEviction => Error::Inactive,
        MlsGroupStateError::LibraryError(_) => Error::Internal,
        _ => Error::Rejected,
    }
}

fn credential_for(identity: &[u8], public_key: &[u8]) -> CredentialWithKey {
    CredentialWithKey {
        credential: BasicCredential::new(identity.to_vec()).into(),
        signature_key: public_key.to_vec().into(),
    }
}

fn put(out: &mut Vec<u8>, bytes: &[u8]) {
    out.extend_from_slice(&(bytes.len() as u32).to_be_bytes());
    out.extend_from_slice(bytes);
}

fn take<'a>(input: &mut &'a [u8]) -> Result<&'a [u8]> {
    if input.len() < 4 {
        return Err(Error::Malformed);
    }
    let (len, rest) = input.split_at(4);
    let len = u32::from_be_bytes([len[0], len[1], len[2], len[3]]) as usize;
    if rest.len() < len {
        return Err(Error::Malformed);
    }
    let (value, rest) = rest.split_at(len);
    *input = rest;
    Ok(value)
}

impl Client {
    /// A new device whose credential carries `identity`, 1 to 64 bytes.
    pub fn new(identity: &[u8]) -> Result<Self> {
        check_id(identity)?;
        let signer = SignatureKeyPair::new(CIPHERSUITE.signature_algorithm())
            .map_err(|_| Error::Internal)?;
        let provider = OpenMlsRustCrypto::default();
        // Kept in the store too, so exported state holds it.
        signer
            .store(provider.storage())
            .map_err(|_| Error::Internal)?;
        Ok(Client {
            credential: credential_for(identity, signer.public()),
            provider,
            signer,
            identity: identity.to_vec(),
        })
    }

    pub fn identity(&self) -> &[u8] {
        &self.identity
    }

    /// A fresh single-use KeyPackage, serialised as an MLSMessage. Its private init key stays in
    /// the store until a welcome consumes it.
    pub fn key_package(&self) -> Result<Vec<u8>> {
        let bundle = KeyPackage::builder()
            .build(
                CIPHERSUITE,
                &self.provider,
                &self.signer,
                self.credential.clone(),
            )
            .map_err(|_| Error::Internal)?;
        serialize(&MlsMessageOut::from(bundle.key_package().clone()))
    }

    /// A group at epoch 0 with this device its only member.
    pub fn create_group(&self, group_id: &[u8]) -> Result<MlsGroup> {
        check_id(group_id)?;
        // The welcome carries the ratchet tree, so joining needs no second message.
        let config = MlsGroupCreateConfig::builder()
            .ciphersuite(CIPHERSUITE)
            .use_ratchet_tree_extension(true)
            .build();
        MlsGroup::new_with_group_id(
            &self.provider,
            &self.signer,
            &config,
            GroupId::from_slice(group_id),
            self.credential.clone(),
        )
        .map_err(|e| match e {
            NewGroupError::GroupAlreadyExists => Error::Rejected,
            _ => Error::Internal,
        })
    }

    /// Joins the group a welcome invites this device into. `Rejected` when the welcome names
    /// none of this device's key packages: it was for someone else.
    pub fn join(&self, welcome: &[u8]) -> Result<MlsGroup> {
        let MlsMessageBodyIn::Welcome(welcome) = decode(welcome)? else {
            return Err(Error::Malformed);
        };
        let config = MlsGroupJoinConfig::builder()
            .use_ratchet_tree_extension(true)
            .build();
        let staged = StagedWelcome::new_from_welcome(&self.provider, &config, welcome, None)
            .map_err(|e| match e {
                WelcomeError::LibraryError(_) | WelcomeError::StorageError(_) => Error::Internal,
                _ => Error::Rejected,
            })?;
        staged
            .into_group(&self.provider)
            .map_err(|_| Error::Internal)
    }

    /// A group this device is in, from the store: after `import_state`, say.
    pub fn load_group(&self, group_id: &[u8]) -> Result<MlsGroup> {
        MlsGroup::load(self.provider.storage(), &GroupId::from_slice(group_id))
            .map_err(|_| Error::Internal)?
            .ok_or(Error::NotAMember)
    }

    /// Removes a group's state from the store.
    pub fn forget_group(&self, mut group: MlsGroup) -> Result<()> {
        group
            .delete(self.provider.storage())
            .map_err(|_| Error::Internal)
    }

    /// Everything this device holds, as bytes to keep (IndexedDB takes a Uint8Array as is):
    /// its identity, its signature key, its unused key packages' private keys and every
    /// group's state. They are secret; whoever has them is this device.
    pub fn export_state(&self) -> Result<Vec<u8>> {
        let values = self
            .provider
            .storage()
            .values
            .read()
            .map_err(|_| Error::Internal)?;
        let mut entries: Vec<_> = values.iter().collect();
        // Sorted, so the same state exports as the same bytes.
        entries.sort();
        let mut out = STATE_MAGIC.to_vec();
        put(&mut out, &self.identity);
        put(&mut out, self.signer.public());
        out.extend_from_slice(&(entries.len() as u32).to_be_bytes());
        for (key, value) in entries {
            put(&mut out, key);
            put(&mut out, value);
        }
        Ok(out)
    }

    /// The device `export_state` saved.
    pub fn import_state(state: &[u8]) -> Result<Self> {
        let mut input = state
            .strip_prefix(STATE_MAGIC.as_slice())
            .ok_or(Error::Malformed)?;
        let identity = take(&mut input)?.to_vec();
        check_id(&identity).map_err(|_| Error::Malformed)?;
        let public = take(&mut input)?.to_vec();
        if input.len() < 4 {
            return Err(Error::Malformed);
        }
        let (count, rest) = input.split_at(4);
        input = rest;
        let count = u32::from_be_bytes([count[0], count[1], count[2], count[3]]) as usize;
        let mut values = HashMap::with_capacity(count.min(4096));
        for _ in 0..count {
            let key = take(&mut input)?.to_vec();
            let value = take(&mut input)?.to_vec();
            values.insert(key, value);
        }
        if !input.is_empty() {
            return Err(Error::Malformed);
        }
        let provider = OpenMlsRustCrypto::default();
        *provider
            .storage()
            .values
            .write()
            .map_err(|_| Error::Internal)? = values;
        let signer = SignatureKeyPair::read(
            provider.storage(),
            &public,
            CIPHERSUITE.signature_algorithm(),
        )
        .ok_or(Error::Malformed)?;
        Ok(Client {
            credential: credential_for(&identity, &public),
            provider,
            signer,
            identity,
        })
    }

    /// Commits adding the members whose key packages are given, and returns the commit and the
    /// welcome. The commit stays pending: send the commit to the group, and once the room has
    /// sequenced it ahead of any other commit for this epoch, `merge_pending_commit` and send
    /// the welcome; if another commit won, `clear_pending_commit`.
    pub fn add(&self, group: &mut MlsGroup, key_packages: &[&[u8]]) -> Result<(Vec<u8>, Vec<u8>)> {
        if key_packages.is_empty() {
            return Err(Error::InvalidArgument);
        }
        let mut packages = Vec::with_capacity(key_packages.len());
        for bytes in key_packages {
            let MlsMessageBodyIn::KeyPackage(package) = decode(bytes)? else {
                return Err(Error::Malformed);
            };
            let package = package
                .validate(self.provider.crypto(), ProtocolVersion::Mls10)
                .map_err(|_| Error::Rejected)?;
            packages.push(package);
        }
        let (commit, welcome, _) = group
            .add_members(&self.provider, &self.signer, &packages)
            .map_err(|e| match e {
                AddMembersError::GroupStateError(state) => state_error(&state),
                AddMembersError::LibraryError(_) | AddMembersError::StorageError(_) => {
                    Error::Internal
                }
                _ => Error::Rejected,
            })?;
        let out = serialize(&commit).and_then(|c| Ok((c, serialize(&welcome)?)));
        if out.is_err() {
            let _ = group.clear_pending_commit(self.provider.storage());
        }
        out
    }

    /// Commits removing the member whose credential carries `identity`. Pending, as for `add`.
    pub fn remove(&self, group: &mut MlsGroup, identity: &[u8]) -> Result<Vec<u8>> {
        check_id(identity)?;
        let member = group
            .members()
            .find(|m| m.credential.serialized_content() == identity)
            .ok_or(Error::NotAMember)?;
        let (commit, _, _) = group
            .remove_members(&self.provider, &self.signer, &[member.index])
            .map_err(|e| match e {
                RemoveMembersError::GroupStateError(state) => state_error(&state),
                RemoveMembersError::LibraryError(_) | RemoveMembersError::StorageError(_) => {
                    Error::Internal
                }
                _ => Error::Rejected,
            })?;
        let out = serialize(&commit);
        if out.is_err() {
            let _ = group.clear_pending_commit(self.provider.storage());
        }
        out
    }

    /// Applies this device's pending commit: the room sequenced it first for its epoch.
    pub fn merge_pending_commit(&self, group: &mut MlsGroup) -> Result<()> {
        if group.pending_commit().is_none() {
            return Err(Error::Rejected);
        }
        group
            .merge_pending_commit(&self.provider)
            .map_err(|e| match e {
                MergePendingCommitError::MlsGroupStateError(state) => state_error(&state),
                _ => Error::Internal,
            })
    }

    /// Drops this device's pending commit: another commit won the epoch.
    pub fn clear_pending_commit(&self, group: &mut MlsGroup) -> Result<()> {
        group
            .clear_pending_commit(self.provider.storage())
            .map_err(|_| Error::Internal)
    }

    /// An application message for the group's current epoch.
    pub fn encrypt(&self, group: &mut MlsGroup, plaintext: &[u8]) -> Result<Vec<u8>> {
        let message = group
            .create_message(&self.provider, &self.signer, plaintext)
            .map_err(|e| match e {
                CreateMessageError::GroupStateError(state) => state_error(&state),
                CreateMessageError::LibraryError(_) => Error::Internal,
            })?;
        serialize(&message)
    }

    /// A message another member sent to the group: application messages decrypt, commits are
    /// merged (the room has already ordered them), proposals are queued. A member never
    /// processes its own messages.
    pub fn process(&self, group: &mut MlsGroup, message: &[u8]) -> Result<Received> {
        let protocol_message = MlsMessageIn::tls_deserialize_exact(message)
            .map_err(|_| Error::Malformed)?
            .try_into_protocol_message()
            .map_err(|_| Error::Malformed)?;
        let processed = group
            .process_message(&self.provider, protocol_message)
            .map_err(|e| match e {
                ProcessMessageError::GroupStateError(state) => state_error(&state),
                ProcessMessageError::LibraryError(_) | ProcessMessageError::StorageError(_) => {
                    Error::Internal
                }
                _ => Error::Rejected,
            })?;
        let sender = processed.credential().serialized_content().to_vec();
        let (kind, plaintext) = match processed.into_content() {
            ProcessedMessageContent::ApplicationMessage(application) => {
                (Kind::Application, application.into_bytes())
            }
            ProcessedMessageContent::ProposalMessage(proposal) => {
                group
                    .store_pending_proposal(self.provider.storage(), *proposal)
                    .map_err(|_| Error::Internal)?;
                (Kind::Proposal, Vec::new())
            }
            ProcessedMessageContent::StagedCommitMessage(commit) => {
                group
                    .merge_staged_commit(&self.provider, *commit)
                    .map_err(|_| Error::Internal)?;
                (Kind::Commit, Vec::new())
            }
            ProcessedMessageContent::ExternalJoinProposalMessage(_)
            | ProcessedMessageContent::OwnPendingCommit
            | ProcessedMessageContent::OwnPrivateMessage => return Err(Error::Rejected),
        };
        Ok(Received {
            kind,
            plaintext,
            sender,
        })
    }
}

/// The identities of a group's members, in leaf order.
pub fn members(group: &MlsGroup) -> Vec<Vec<u8>> {
    group
        .members()
        .map(|m| m.credential.serialized_content().to_vec())
        .collect()
}

/// Reads what kind of MLSMessage `bytes` is, and for a key package checks its signature.
pub fn inspect(bytes: &[u8]) -> Result<Info> {
    let message = MlsMessageIn::tls_deserialize_exact(bytes).map_err(|_| Error::Malformed)?;
    let mut info = Info {
        wire_format: "",
        group_id: None,
        epoch: None,
        content_type: None,
        identity: None,
    };
    match message.extract() {
        MlsMessageBodyIn::PublicMessage(m) => {
            let m = ProtocolMessage::from(m);
            info.wire_format = "public_message";
            protocol_info(&mut info, &m);
        }
        MlsMessageBodyIn::PrivateMessage(m) => {
            let m = ProtocolMessage::from(m);
            info.wire_format = "private_message";
            protocol_info(&mut info, &m);
        }
        MlsMessageBodyIn::Welcome(_) => info.wire_format = "welcome",
        MlsMessageBodyIn::GroupInfo(_) => info.wire_format = "group_info",
        MlsMessageBodyIn::KeyPackage(package) => {
            info.wire_format = "key_package";
            let crypto = openmls_rust_crypto::RustCrypto::default();
            let package = package
                .validate(&crypto, ProtocolVersion::Mls10)
                .map_err(|_| Error::Rejected)?;
            if package.ciphersuite() != CIPHERSUITE {
                return Err(Error::Rejected);
            }
            info.identity = Some(
                package
                    .leaf_node()
                    .credential()
                    .serialized_content()
                    .to_vec(),
            );
        }
    }
    Ok(info)
}

fn protocol_info(info: &mut Info, message: &ProtocolMessage) {
    info.group_id = Some(message.group_id().as_slice().to_vec());
    info.epoch = Some(message.epoch().as_u64());
    info.content_type = Some(match message.content_type() {
        ContentType::Application => "application",
        ContentType::Proposal => "proposal",
        ContentType::Commit => "commit",
    });
}
