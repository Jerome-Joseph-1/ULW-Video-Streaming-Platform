//! The JavaScript API. Bytes in and out are Uint8Arrays; a failure throws an Error whose message
//! is the bridge's status name (`malformed`, `rejected`, `not_a_member`, `inactive`,
//! `invalid_argument`, `internal`).

use std::cell::RefCell;
use std::collections::HashMap;
use std::rc::Rc;

use js_sys::Uint8Array;
use openmls::prelude::MlsGroup;
use wasm_bindgen::prelude::*;

use crate::core::{self, Client, Error, Kind};

fn js(error: Error) -> JsError {
    JsError::new(error.as_str())
}

type Groups = Rc<RefCell<HashMap<Vec<u8>, Rc<RefCell<MlsGroup>>>>>;

/// A device: its identity, signature key, key packages and groups. Each group has one state
/// in memory however many `MlsGroup` handles reach it, so two handles never diverge.
#[wasm_bindgen(js_name = MlsClient)]
pub struct JsClient {
    inner: Rc<Client>,
    groups: Groups,
}

impl JsClient {
    fn wrap(client: Client) -> JsClient {
        JsClient {
            inner: Rc::new(client),
            groups: Rc::default(),
        }
    }

    fn handle(&self, group: MlsGroup) -> JsGroup {
        let id = group.group_id().as_slice().to_vec();
        let group = Rc::new(RefCell::new(group));
        self.groups.borrow_mut().insert(id, Rc::clone(&group));
        JsGroup {
            client: Rc::clone(&self.inner),
            groups: Rc::clone(&self.groups),
            group,
        }
    }
}

#[wasm_bindgen(js_class = MlsClient)]
impl JsClient {
    /// A new device whose basic credential carries `identity` (1 to 64 bytes: its device id).
    #[wasm_bindgen(constructor)]
    pub fn new(identity: &[u8]) -> Result<JsClient, JsError> {
        Ok(JsClient::wrap(Client::new(identity).map_err(js)?))
    }

    /// The device `exportState` saved, groups and unused key packages included.
    #[wasm_bindgen(js_name = importState)]
    pub fn import_state(state: &[u8]) -> Result<JsClient, JsError> {
        Ok(JsClient::wrap(Client::import_state(state).map_err(js)?))
    }

    #[wasm_bindgen(getter)]
    pub fn identity(&self) -> Vec<u8> {
        self.inner.identity().to_vec()
    }

    /// SHA-256 of this device's signature public key, hex, for comparing out of band.
    #[wasm_bindgen(getter)]
    pub fn fingerprint(&self) -> Result<String, JsError> {
        self.inner.fingerprint().map_err(js)
    }

    /// The application's bytes kept with the state (mls-room.js uses them).
    #[wasm_bindgen(getter, js_name = appData)]
    pub fn app_data(&self) -> Vec<u8> {
        self.inner.app_data()
    }

    #[wasm_bindgen(setter, js_name = appData)]
    pub fn set_app_data(&self, data: &[u8]) {
        self.inner.set_app_data(data);
    }

    /// A fresh single-use KeyPackage as an MLSMessage: post it to the room for a member to add.
    #[wasm_bindgen(js_name = keyPackage)]
    pub fn key_package(&self) -> Result<Vec<u8>, JsError> {
        self.inner.key_package().map_err(js)
    }

    /// A new group, at epoch 0, with this device its only member. The chat convention is the
    /// room id's text (36 bytes) as the group id.
    #[wasm_bindgen(js_name = createGroup)]
    pub fn create_group(&self, group_id: &[u8]) -> Result<JsGroup, JsError> {
        let group = self.inner.create_group(group_id).map_err(js)?;
        Ok(self.handle(group))
    }

    /// Joins the group a Welcome invites this device into; throws `rejected` when the welcome
    /// is for other devices, is for a group other than `expectedGroupId` (when given), or for
    /// a group this device is in already. A refused welcome leaves nothing behind.
    #[wasm_bindgen(js_name = joinGroup)]
    pub fn join_group(
        &self,
        welcome: &[u8],
        expected_group_id: Option<Vec<u8>>,
    ) -> Result<JsGroup, JsError> {
        let group = self
            .inner
            .join(welcome, expected_group_id.as_deref())
            .map_err(js)?;
        Ok(self.handle(group))
    }

    /// A group this device is in, from its state; throws `not_a_member` when there is none.
    #[wasm_bindgen(js_name = loadGroup)]
    pub fn load_group(&self, group_id: &[u8]) -> Result<JsGroup, JsError> {
        if let Some(group) = self.groups.borrow().get(group_id) {
            return Ok(JsGroup {
                client: Rc::clone(&self.inner),
                groups: Rc::clone(&self.groups),
                group: Rc::clone(group),
            });
        }
        let group = self.inner.load_group(group_id).map_err(js)?;
        Ok(self.handle(group))
    }

    /// Everything this device holds, as bytes for IndexedDB. Secret: whoever has them is this
    /// device. Export again after every call that changes a group or makes a key package.
    #[wasm_bindgen(js_name = exportState)]
    pub fn export_state(&self) -> Result<Vec<u8>, JsError> {
        self.inner.export_state().map_err(js)
    }
}

/// What `MlsGroup.add` made.
#[wasm_bindgen]
pub struct AddResult {
    commit: Vec<u8>,
    welcome: Vec<u8>,
}

#[wasm_bindgen]
impl AddResult {
    /// For the room first. Merge once the room sequenced it, then send the welcome.
    #[wasm_bindgen(getter)]
    pub fn commit(&self) -> Vec<u8> {
        self.commit.clone()
    }

    #[wasm_bindgen(getter)]
    pub fn welcome(&self) -> Vec<u8> {
        self.welcome.clone()
    }
}

/// What `MlsGroup.process` found.
#[wasm_bindgen]
pub struct Received {
    kind: Kind,
    plaintext: Vec<u8>,
    sender: Vec<u8>,
    sender_leaf: Option<u32>,
    self_removed: bool,
}

#[wasm_bindgen]
impl Received {
    /// `application`, `commit` (merged: the group is at the next epoch) or `proposal`.
    #[wasm_bindgen(getter)]
    pub fn kind(&self) -> String {
        match self.kind {
            Kind::Application => "application",
            Kind::Commit => "commit",
            Kind::Proposal => "proposal",
        }
        .to_owned()
    }

    /// An application message's plaintext; empty for the other kinds.
    #[wasm_bindgen(getter)]
    pub fn plaintext(&self) -> Vec<u8> {
        self.plaintext.clone()
    }

    /// The identity in the sender's credential: for a commit, the committer's.
    #[wasm_bindgen(getter)]
    pub fn sender(&self) -> Vec<u8> {
        self.sender.clone()
    }

    /// The sender's leaf index.
    #[wasm_bindgen(getter, js_name = senderLeaf)]
    pub fn sender_leaf(&self) -> Option<u32> {
        self.sender_leaf
    }

    /// The commit removed this device; the group can no longer be used.
    #[wasm_bindgen(getter, js_name = selfRemoved)]
    pub fn self_removed(&self) -> bool {
        self.self_removed
    }
}

/// A group member: `{leaf, identity, fingerprint}`.
#[wasm_bindgen]
pub struct Member {
    info: core::MemberInfo,
}

#[wasm_bindgen]
impl Member {
    #[wasm_bindgen(getter)]
    pub fn leaf(&self) -> u32 {
        self.info.leaf
    }

    #[wasm_bindgen(getter)]
    pub fn identity(&self) -> Vec<u8> {
        self.info.identity.clone()
    }

    /// SHA-256 of its signature public key, hex.
    #[wasm_bindgen(getter)]
    pub fn fingerprint(&self) -> String {
        self.info.fingerprint.clone()
    }
}

/// What a room message's body is, read without keys.
#[wasm_bindgen]
pub struct MessageInfo {
    info: core::Info,
}

#[wasm_bindgen]
impl MessageInfo {
    /// `key_package`, `welcome`, `private_message`, `public_message` or `group_info`.
    #[wasm_bindgen(getter, js_name = wireFormat)]
    pub fn wire_format(&self) -> String {
        self.info.wire_format.to_owned()
    }

    /// A public or private message's group id.
    #[wasm_bindgen(getter, js_name = groupId)]
    pub fn group_id(&self) -> Option<Vec<u8>> {
        self.info.group_id.clone()
    }

    /// A public or private message's epoch (exact below 2^53).
    #[wasm_bindgen(getter)]
    pub fn epoch(&self) -> Option<f64> {
        self.info.epoch.map(|e| e as f64)
    }

    /// A public or private message's `application`, `proposal` or `commit`.
    #[wasm_bindgen(getter, js_name = contentType)]
    pub fn content_type(&self) -> Option<String> {
        self.info.content_type.map(str::to_owned)
    }

    /// A key package's identity; its signature and ciphersuite have been checked.
    #[wasm_bindgen(getter)]
    pub fn identity(&self) -> Option<Vec<u8>> {
        self.info.identity.clone()
    }

    /// A key package's signature key fingerprint: SHA-256, hex.
    #[wasm_bindgen(getter)]
    pub fn fingerprint(&self) -> Option<String> {
        self.info.fingerprint.clone()
    }

    /// A key package's KeyPackageRef, hex: the same package always has the same one.
    #[wasm_bindgen(getter, js_name = keyPackageRef)]
    pub fn key_package_ref(&self) -> Option<String> {
        self.info.key_package_ref.clone()
    }
}

/// Reads what kind of MLSMessage a body is. Throws `malformed` for anything else, and
/// `rejected` for a key package whose signature or ciphersuite is wrong.
#[wasm_bindgen]
pub fn inspect(bytes: &[u8]) -> Result<MessageInfo, JsError> {
    Ok(MessageInfo {
        info: core::inspect(bytes).map_err(js)?,
    })
}

/// The ciphersuite every group and key package uses, by its RFC 9420 code point (1).
#[wasm_bindgen]
pub fn ciphersuite() -> u16 {
    core::CIPHERSUITE as u16
}

/// One device's view of one group. Its state is in the client's store as it changes, so the
/// client's `exportState` covers it.
#[wasm_bindgen(js_name = MlsGroup)]
pub struct JsGroup {
    client: Rc<Client>,
    groups: Groups,
    group: Rc<RefCell<MlsGroup>>,
}

#[wasm_bindgen(js_class = MlsGroup)]
impl JsGroup {
    #[wasm_bindgen(getter, js_name = groupId)]
    pub fn group_id(&self) -> Vec<u8> {
        self.group.borrow().group_id().as_slice().to_vec()
    }

    #[wasm_bindgen(getter)]
    pub fn epoch(&self) -> f64 {
        self.group.borrow().epoch().as_u64() as f64
    }

    /// The members, in leaf order: `[{leaf, identity, fingerprint}]`.
    pub fn members(&self) -> Result<Vec<Member>, JsError> {
        Ok(core::members(&self.group.borrow())
            .map_err(js)?
            .into_iter()
            .map(|info| Member { info })
            .collect())
    }

    /// Whether the group can still be used: false once a commit removed this device.
    #[wasm_bindgen(getter)]
    pub fn active(&self) -> bool {
        self.group.borrow().is_active()
    }

    #[wasm_bindgen(getter, js_name = memberCount)]
    pub fn member_count(&self) -> usize {
        self.group.borrow().members().count()
    }

    #[wasm_bindgen(getter, js_name = hasPendingCommit)]
    pub fn has_pending_commit(&self) -> bool {
        self.group.borrow().pending_commit().is_some()
    }

    /// Commits adding the devices whose key packages are given (an array of Uint8Array).
    /// The commit stays pending until `mergePendingCommit` or `clearPendingCommit`.
    pub fn add(&self, key_packages: Vec<Uint8Array>) -> Result<AddResult, JsError> {
        let owned: Vec<Vec<u8>> = key_packages.iter().map(Uint8Array::to_vec).collect();
        let packages: Vec<&[u8]> = owned.iter().map(Vec::as_slice).collect();
        let (commit, welcome) = self
            .client
            .add(&mut self.group.borrow_mut(), &packages)
            .map_err(js)?;
        Ok(AddResult { commit, welcome })
    }

    /// Commits removing the member whose credential carries `identity`. Pending, as for `add`.
    pub fn remove(&self, identity: &[u8]) -> Result<Vec<u8>, JsError> {
        self.client
            .remove(&mut self.group.borrow_mut(), identity)
            .map_err(js)
    }

    #[wasm_bindgen(js_name = mergePendingCommit)]
    pub fn merge_pending_commit(&self) -> Result<(), JsError> {
        self.client
            .merge_pending_commit(&mut self.group.borrow_mut())
            .map_err(js)
    }

    #[wasm_bindgen(js_name = clearPendingCommit)]
    pub fn clear_pending_commit(&self) -> Result<(), JsError> {
        self.client
            .clear_pending_commit(&mut self.group.borrow_mut())
            .map_err(js)
    }

    /// An application message (a PrivateMessage) for the current epoch.
    pub fn encrypt(&self, plaintext: &[u8]) -> Result<Vec<u8>, JsError> {
        self.client
            .encrypt(&mut self.group.borrow_mut(), plaintext)
            .map_err(js)
    }

    /// Another member's message: decrypts an application message, merges a commit, queues a
    /// proposal. Never pass this device's own messages back in.
    pub fn process(&self, message: &[u8]) -> Result<Received, JsError> {
        let r = self
            .client
            .process(&mut self.group.borrow_mut(), message)
            .map_err(js)?;
        Ok(Received {
            kind: r.kind,
            plaintext: r.plaintext,
            sender: r.sender,
            sender_leaf: r.sender_leaf,
            self_removed: r.self_removed,
        })
    }

    /// Deletes the group's state from the client's store; every handle to it is unusable
    /// afterwards.
    pub fn forget(self) -> Result<(), JsError> {
        let id = self.group.borrow().group_id().as_slice().to_vec();
        self.groups.borrow_mut().remove(&id);
        self.client
            .forget_group(&mut self.group.borrow_mut())
            .map_err(js)
    }
}
