//! The client, in plain Rust: what `wasm.rs` exposes to JavaScript, and what the native tests
//! drive. Every choice that reaches the wire is the FFI bridge's (infra/e2ee/mls_ffi_bridge,
//! ADR-0044), so the two interoperate: the ciphersuite, basic credentials carrying the device's
//! identity, the ratchet tree in the welcome, OpenMLS's default wire format policy, and every
//! message serialised as an MLSMessage (RFC 9420, section 6).

use std::cell::RefCell;
use std::collections::HashMap;

use openmls::prelude::tls_codec::Deserialize as _;
use openmls::prelude::*;
use openmls_basic_credential::SignatureKeyPair;
use openmls_rust_crypto::OpenMlsRustCrypto;
use openmls_traits::OpenMlsProvider;
use openmls_traits::crypto::OpenMlsCrypto;
use openmls_traits::types::HashType;

/// The suite RFC 9420 makes mandatory, and the only one the bridge speaks.
pub const CIPHERSUITE: Ciphersuite = Ciphersuite::MLS_128_DHKEMX25519_AES128GCM_SHA256_Ed25519;

/// A device id or a group id: 1 to 64 bytes, as the bridge allows.
pub const MAX_IDENTITY: usize = 64;

/// How many past epochs' message secrets a group keeps, so an application message sent just
/// before a commit, and sequenced after it, still decrypts. Local: nothing on the wire changes.
pub const MAX_PAST_EPOCHS: usize = 4;

// Exported state starts with this and a format version: 1 had no application data.
const STATE_MAGIC: &[u8; 7] = b"ULWMLS\0";
const STATE_VERSION: u8 = 2;

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
    /// The identity in the sender's credential: for a commit, its committer.
    pub sender: Vec<u8>,
    /// The sender's leaf index.
    pub sender_leaf: Option<u32>,
    /// A commit removed this device: the group can no longer be used.
    pub self_removed: bool,
}

/// A member of a group, as this device sees it.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct MemberInfo {
    pub leaf: u32,
    pub identity: Vec<u8>,
    /// SHA-256 of its signature public key, hex: what two people compare out of band.
    pub fingerprint: String,
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
    /// For a key package: SHA-256 of its signature public key, hex.
    pub fingerprint: Option<String>,
    /// For a key package: its KeyPackageRef (RFC 9420, section 5.2), hex.
    pub key_package_ref: Option<String>,
}

fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

/// SHA-256 of a signature public key, hex.
pub fn fingerprint(signature_key: &[u8]) -> Result<String> {
    let crypto = openmls_rust_crypto::RustCrypto::default();
    let digest = crypto
        .hash(HashType::Sha2_256, signature_key)
        .map_err(|_| Error::Internal)?;
    Ok(hex(&digest))
}

/// A device's MLS identity: its signature key, its credential, and a store holding the private
/// halves of its key packages and every group's state.
pub struct Client {
    provider: OpenMlsRustCrypto,
    signer: SignatureKeyPair,
    credential: CredentialWithKey,
    identity: Vec<u8>,
    // The application's own bytes, kept with the state (mls-room.js keeps its outstanding
    // commit and the key packages it has decided on here).
    app_data: RefCell<Vec<u8>>,
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
            app_data: RefCell::default(),
        })
    }

    pub fn app_data(&self) -> Vec<u8> {
        self.app_data.borrow().clone()
    }

    /// Bytes the application keeps with the state; `export_state` includes them.
    pub fn set_app_data(&self, data: &[u8]) {
        *self.app_data.borrow_mut() = data.to_vec();
    }

    /// SHA-256 of this device's signature public key, hex.
    pub fn fingerprint(&self) -> Result<String> {
        fingerprint(self.signer.public())
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
            .max_past_epochs(MAX_PAST_EPOCHS)
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
    /// none of this device's key packages (it was for someone else), when `expected_group`
    /// is given and the welcome's group is another, or when this device is in that group
    /// already. A refused welcome leaves no group behind.
    pub fn join(&self, welcome: &[u8], expected_group: Option<&[u8]>) -> Result<MlsGroup> {
        let MlsMessageBodyIn::Welcome(welcome) = decode(welcome)? else {
            return Err(Error::Malformed);
        };
        let config = MlsGroupJoinConfig::builder()
            .use_ratchet_tree_extension(true)
            .max_past_epochs(MAX_PAST_EPOCHS)
            .build();
        let staged = StagedWelcome::new_from_welcome(&self.provider, &config, welcome, None)
            .map_err(|e| match e {
                WelcomeError::LibraryError(_) | WelcomeError::StorageError(_) => Error::Internal,
                _ => Error::Rejected,
            })?;
        let group_id = staged.group_context().group_id().clone();
        if expected_group.is_some_and(|g| g != group_id.as_slice()) {
            return Err(Error::Rejected);
        }
        let existing =
            MlsGroup::load(self.provider.storage(), &group_id).map_err(|_| Error::Internal)?;
        if existing.is_some() {
            return Err(Error::Rejected);
        }
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
    pub fn forget_group(&self, group: &mut MlsGroup) -> Result<()> {
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
        out.push(STATE_VERSION);
        put(&mut out, &self.identity);
        put(&mut out, self.signer.public());
        put(&mut out, &self.app_data.borrow());
        out.extend_from_slice(&(entries.len() as u32).to_be_bytes());
        for (key, value) in entries {
            put(&mut out, key);
            put(&mut out, value);
        }
        Ok(out)
    }

    /// The device `export_state` saved.
    pub fn import_state(state: &[u8]) -> Result<Self> {
        let input = state
            .strip_prefix(STATE_MAGIC.as_slice())
            .ok_or(Error::Malformed)?;
        let (&version, mut input) = input.split_first().ok_or(Error::Malformed)?;
        if version != 1 && version != STATE_VERSION {
            return Err(Error::Malformed);
        }
        let identity = take(&mut input)?.to_vec();
        check_id(&identity).map_err(|_| Error::Malformed)?;
        let public = take(&mut input)?.to_vec();
        let app_data = if version >= 2 {
            take(&mut input)?.to_vec()
        } else {
            Vec::new()
        };
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
            app_data: RefCell::new(app_data),
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
        // Nothing another member proposed rides along: process() refuses proposals, and this
        // keeps it so if any were queued before.
        group
            .clear_pending_proposals(self.provider.storage())
            .map_err(|_| Error::Internal)?;
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

    /// A message another member sent to the group: application messages decrypt, and commits
    /// are merged (the room has already ordered them). Refused (`Rejected`), and left
    /// unapplied: a standalone proposal, which nothing here sends and which would otherwise
    /// ride along with the next commit; and a commit that adds members made by anyone but
    /// the group's first member, the one the room convention lets add (ADR-0098). A member
    /// never processes its own messages.
    pub fn process(&self, group: &mut MlsGroup, message: &[u8]) -> Result<Received> {
        let protocol_message = MlsMessageIn::tls_deserialize_exact(message)
            .map_err(|_| Error::Malformed)?
            .try_into_protocol_message()
            .map_err(|_| Error::Malformed)?;
        if protocol_message.content_type() == ContentType::Proposal {
            return Err(Error::Rejected);
        }
        let first_leaf = group.members().next().map(|m| m.index);
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
        let sender_leaf = match processed.sender() {
            Sender::Member(leaf) => Some(*leaf),
            _ => None,
        };
        let mut self_removed = false;
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
                if commit.add_proposals().next().is_some() && sender_leaf != first_leaf {
                    return Err(Error::Rejected);
                }
                self_removed = commit.self_removed();
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
            sender_leaf: sender_leaf.map(|l| l.u32()),
            self_removed,
        })
    }
}

/// A group's members, in leaf order.
pub fn members(group: &MlsGroup) -> Result<Vec<MemberInfo>> {
    group
        .members()
        .map(|m| {
            Ok(MemberInfo {
                leaf: m.index.u32(),
                identity: m.credential.serialized_content().to_vec(),
                fingerprint: fingerprint(&m.signature_key)?,
            })
        })
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
        fingerprint: None,
        key_package_ref: None,
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
            info.fingerprint = Some(fingerprint(package.leaf_node().signature_key().as_slice())?);
            info.key_package_ref = Some(hex(package
                .hash_ref(&crypto)
                .map_err(|_| Error::Internal)?
                .as_slice()));
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
