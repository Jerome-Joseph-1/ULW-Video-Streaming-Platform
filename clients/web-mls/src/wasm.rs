//! The JavaScript API. Bytes in and out are Uint8Arrays; a failure throws an Error whose message
//! is the bridge's status name (`malformed`, `rejected`, `not_a_member`, `inactive`,
//! `invalid_argument`, `internal`).

use std::cell::RefCell;
use std::rc::Rc;

use js_sys::Uint8Array;
use openmls::prelude::MlsGroup;
use wasm_bindgen::prelude::*;

use crate::core::{self, Client, Error, Kind};

fn js(error: Error) -> JsError {
    JsError::new(error.as_str())
}

/// A device: its identity, signature key, key packages and groups.
#[wasm_bindgen(js_name = MlsClient)]
pub struct JsClient {
    inner: Rc<Client>,
}

#[wasm_bindgen(js_class = MlsClient)]
impl JsClient {
    /// A new device whose basic credential carries `identity` (1 to 64 bytes: its device id).
    #[wasm_bindgen(constructor)]
    pub fn new(identity: &[u8]) -> Result<JsClient, JsError> {
        Ok(JsClient {
            inner: Rc::new(Client::new(identity).map_err(js)?),
        })
    }

    /// The device `exportState` saved, groups and unused key packages included.
    #[wasm_bindgen(js_name = importState)]
    pub fn import_state(state: &[u8]) -> Result<JsClient, JsError> {
        Ok(JsClient {
            inner: Rc::new(Client::import_state(state).map_err(js)?),
        })
    }

    #[wasm_bindgen(getter)]
    pub fn identity(&self) -> Vec<u8> {
        self.inner.identity().to_vec()
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
        Ok(JsGroup::wrap(&self.inner, group))
    }

    /// Joins the group a Welcome invites this device into; throws `rejected` when the welcome
    /// is for other devices.
    #[wasm_bindgen(js_name = joinGroup)]
    pub fn join_group(&self, welcome: &[u8]) -> Result<JsGroup, JsError> {
        let group = self.inner.join(welcome).map_err(js)?;
        Ok(JsGroup::wrap(&self.inner, group))
    }

    /// A group this device is in, from its state; throws `not_a_member` when there is none.
    #[wasm_bindgen(js_name = loadGroup)]
    pub fn load_group(&self, group_id: &[u8]) -> Result<JsGroup, JsError> {
        let group = self.inner.load_group(group_id).map_err(js)?;
        Ok(JsGroup::wrap(&self.inner, group))
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

    /// The identity in the sender's credential.
    #[wasm_bindgen(getter)]
    pub fn sender(&self) -> Vec<u8> {
        self.sender.clone()
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
    group: RefCell<MlsGroup>,
}

impl JsGroup {
    fn wrap(client: &Rc<Client>, group: MlsGroup) -> JsGroup {
        JsGroup {
            client: Rc::clone(client),
            group: RefCell::new(group),
        }
    }
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

    /// Members' identities, in leaf order.
    pub fn members(&self) -> Vec<Uint8Array> {
        core::members(&self.group.borrow())
            .iter()
            .map(|m| Uint8Array::from(m.as_slice()))
            .collect()
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
        })
    }

    /// Deletes the group's state from the client's store; the handle is unusable afterwards.
    pub fn forget(self) -> Result<(), JsError> {
        self.client
            .forget_group(self.group.into_inner())
            .map_err(js)
    }
}
