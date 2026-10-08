/* @ts-self-types="./web_mls.d.ts" */

/**
 * What `MlsGroup.add` made.
 */
export class AddResult {
    static __wrap(ptr) {
        const obj = Object.create(AddResult.prototype);
        obj.__wbg_ptr = ptr;
        AddResultFinalization.register(obj, obj.__wbg_ptr, obj);
        return obj;
    }
    __destroy_into_raw() {
        const ptr = this.__wbg_ptr;
        this.__wbg_ptr = 0;
        AddResultFinalization.unregister(this);
        return ptr;
    }
    free() {
        const ptr = this.__destroy_into_raw();
        wasm.__wbg_addresult_free(ptr, 0);
    }
    /**
     * For the room first. Merge once the room sequenced it, then send the welcome.
     * @returns {Uint8Array}
     */
    get commit() {
        const ret = wasm.addresult_commit(this.__wbg_ptr);
        var v1 = getArrayU8FromWasm0(ret[0], ret[1]).slice();
        wasm.__wbindgen_free(ret[0], ret[1] * 1, 1);
        return v1;
    }
    /**
     * @returns {Uint8Array}
     */
    get welcome() {
        const ret = wasm.addresult_welcome(this.__wbg_ptr);
        var v1 = getArrayU8FromWasm0(ret[0], ret[1]).slice();
        wasm.__wbindgen_free(ret[0], ret[1] * 1, 1);
        return v1;
    }
}
if (Symbol.dispose) AddResult.prototype[Symbol.dispose] = AddResult.prototype.free;

/**
 * A group member: `{leaf, identity, fingerprint}`.
 */
export class Member {
    static __wrap(ptr) {
        const obj = Object.create(Member.prototype);
        obj.__wbg_ptr = ptr;
        MemberFinalization.register(obj, obj.__wbg_ptr, obj);
        return obj;
    }
    __destroy_into_raw() {
        const ptr = this.__wbg_ptr;
        this.__wbg_ptr = 0;
        MemberFinalization.unregister(this);
        return ptr;
    }
    free() {
        const ptr = this.__destroy_into_raw();
        wasm.__wbg_member_free(ptr, 0);
    }
    /**
     * SHA-256 of its signature public key, hex.
     * @returns {string}
     */
    get fingerprint() {
        let deferred1_0;
        let deferred1_1;
        try {
            const ret = wasm.member_fingerprint(this.__wbg_ptr);
            deferred1_0 = ret[0];
            deferred1_1 = ret[1];
            return getStringFromWasm0(ret[0], ret[1]);
        } finally {
            wasm.__wbindgen_free(deferred1_0, deferred1_1, 1);
        }
    }
    /**
     * @returns {Uint8Array}
     */
    get identity() {
        const ret = wasm.member_identity(this.__wbg_ptr);
        var v1 = getArrayU8FromWasm0(ret[0], ret[1]).slice();
        wasm.__wbindgen_free(ret[0], ret[1] * 1, 1);
        return v1;
    }
    /**
     * @returns {number}
     */
    get leaf() {
        const ret = wasm.member_leaf(this.__wbg_ptr);
        return ret >>> 0;
    }
}
if (Symbol.dispose) Member.prototype[Symbol.dispose] = Member.prototype.free;

/**
 * What a room message's body is, read without keys.
 */
export class MessageInfo {
    static __wrap(ptr) {
        const obj = Object.create(MessageInfo.prototype);
        obj.__wbg_ptr = ptr;
        MessageInfoFinalization.register(obj, obj.__wbg_ptr, obj);
        return obj;
    }
    __destroy_into_raw() {
        const ptr = this.__wbg_ptr;
        this.__wbg_ptr = 0;
        MessageInfoFinalization.unregister(this);
        return ptr;
    }
    free() {
        const ptr = this.__destroy_into_raw();
        wasm.__wbg_messageinfo_free(ptr, 0);
    }
    /**
     * A public or private message's `application`, `proposal` or `commit`.
     * @returns {string | undefined}
     */
    get contentType() {
        const ret = wasm.messageinfo_contentType(this.__wbg_ptr);
        let v1;
        if (ret[0] !== 0) {
            v1 = getStringFromWasm0(ret[0], ret[1]);
            wasm.__wbindgen_free(ret[0], ret[1] * 1, 1);
        }
        return v1;
    }
    /**
     * A public or private message's epoch (exact below 2^53).
     * @returns {number | undefined}
     */
    get epoch() {
        const ret = wasm.messageinfo_epoch(this.__wbg_ptr);
        return ret[0] === 0 ? undefined : ret[1];
    }
    /**
     * A key package's signature key fingerprint: SHA-256, hex.
     * @returns {string | undefined}
     */
    get fingerprint() {
        const ret = wasm.messageinfo_fingerprint(this.__wbg_ptr);
        let v1;
        if (ret[0] !== 0) {
            v1 = getStringFromWasm0(ret[0], ret[1]);
            wasm.__wbindgen_free(ret[0], ret[1] * 1, 1);
        }
        return v1;
    }
    /**
     * A public or private message's group id.
     * @returns {Uint8Array | undefined}
     */
    get groupId() {
        const ret = wasm.messageinfo_groupId(this.__wbg_ptr);
        let v1;
        if (ret[0] !== 0) {
            v1 = getArrayU8FromWasm0(ret[0], ret[1]).slice();
            wasm.__wbindgen_free(ret[0], ret[1] * 1, 1);
        }
        return v1;
    }
    /**
     * A key package's identity; its signature and ciphersuite have been checked.
     * @returns {Uint8Array | undefined}
     */
    get identity() {
        const ret = wasm.messageinfo_identity(this.__wbg_ptr);
        let v1;
        if (ret[0] !== 0) {
            v1 = getArrayU8FromWasm0(ret[0], ret[1]).slice();
            wasm.__wbindgen_free(ret[0], ret[1] * 1, 1);
        }
        return v1;
    }
    /**
     * A key package's KeyPackageRef, hex: the same package always has the same one.
     * @returns {string | undefined}
     */
    get keyPackageRef() {
        const ret = wasm.messageinfo_keyPackageRef(this.__wbg_ptr);
        let v1;
        if (ret[0] !== 0) {
            v1 = getStringFromWasm0(ret[0], ret[1]);
            wasm.__wbindgen_free(ret[0], ret[1] * 1, 1);
        }
        return v1;
    }
    /**
     * `key_package`, `welcome`, `private_message`, `public_message` or `group_info`.
     * @returns {string}
     */
    get wireFormat() {
        let deferred1_0;
        let deferred1_1;
        try {
            const ret = wasm.messageinfo_wireFormat(this.__wbg_ptr);
            deferred1_0 = ret[0];
            deferred1_1 = ret[1];
            return getStringFromWasm0(ret[0], ret[1]);
        } finally {
            wasm.__wbindgen_free(deferred1_0, deferred1_1, 1);
        }
    }
}
if (Symbol.dispose) MessageInfo.prototype[Symbol.dispose] = MessageInfo.prototype.free;

/**
 * A device: its identity, signature key, key packages and groups. Each group has one state
 * in memory however many `MlsGroup` handles reach it, so two handles never diverge.
 */
export class MlsClient {
    static __wrap(ptr) {
        const obj = Object.create(MlsClient.prototype);
        obj.__wbg_ptr = ptr;
        MlsClientFinalization.register(obj, obj.__wbg_ptr, obj);
        return obj;
    }
    __destroy_into_raw() {
        const ptr = this.__wbg_ptr;
        this.__wbg_ptr = 0;
        MlsClientFinalization.unregister(this);
        return ptr;
    }
    free() {
        const ptr = this.__destroy_into_raw();
        wasm.__wbg_mlsclient_free(ptr, 0);
    }
    /**
     * The application's bytes kept with the state (mls-room.js uses them).
     * @returns {Uint8Array}
     */
    get appData() {
        const ret = wasm.mlsclient_appData(this.__wbg_ptr);
        var v1 = getArrayU8FromWasm0(ret[0], ret[1]).slice();
        wasm.__wbindgen_free(ret[0], ret[1] * 1, 1);
        return v1;
    }
    /**
     * A new group, at epoch 0, with this device its only member. The chat convention is the
     * room id's text (36 bytes) as the group id.
     * @param {Uint8Array} group_id
     * @returns {MlsGroup}
     */
    createGroup(group_id) {
        const ptr0 = passArray8ToWasm0(group_id, wasm.__wbindgen_malloc);
        const len0 = WASM_VECTOR_LEN;
        const ret = wasm.mlsclient_createGroup(this.__wbg_ptr, ptr0, len0);
        if (ret[2]) {
            throw takeFromExternrefTable0(ret[1]);
        }
        return MlsGroup.__wrap(ret[0]);
    }
    /**
     * Everything this device holds, as bytes for IndexedDB. Secret: whoever has them is this
     * device. Export again after every call that changes a group or makes a key package.
     * @returns {Uint8Array}
     */
    exportState() {
        const ret = wasm.mlsclient_exportState(this.__wbg_ptr);
        if (ret[3]) {
            throw takeFromExternrefTable0(ret[2]);
        }
        var v1 = getArrayU8FromWasm0(ret[0], ret[1]).slice();
        wasm.__wbindgen_free(ret[0], ret[1] * 1, 1);
        return v1;
    }
    /**
     * SHA-256 of this device's signature public key, hex, for comparing out of band.
     * @returns {string}
     */
    get fingerprint() {
        let deferred2_0;
        let deferred2_1;
        try {
            const ret = wasm.mlsclient_fingerprint(this.__wbg_ptr);
            var ptr1 = ret[0];
            var len1 = ret[1];
            if (ret[3]) {
                ptr1 = 0; len1 = 0;
                throw takeFromExternrefTable0(ret[2]);
            }
            deferred2_0 = ptr1;
            deferred2_1 = len1;
            return getStringFromWasm0(ptr1, len1);
        } finally {
            wasm.__wbindgen_free(deferred2_0, deferred2_1, 1);
        }
    }
    /**
     * @returns {Uint8Array}
     */
    get identity() {
        const ret = wasm.mlsclient_identity(this.__wbg_ptr);
        var v1 = getArrayU8FromWasm0(ret[0], ret[1]).slice();
        wasm.__wbindgen_free(ret[0], ret[1] * 1, 1);
        return v1;
    }
    /**
     * The device `exportState` saved, groups and unused key packages included.
     * @param {Uint8Array} state
     * @returns {MlsClient}
     */
    static importState(state) {
        const ptr0 = passArray8ToWasm0(state, wasm.__wbindgen_malloc);
        const len0 = WASM_VECTOR_LEN;
        const ret = wasm.mlsclient_importState(ptr0, len0);
        if (ret[2]) {
            throw takeFromExternrefTable0(ret[1]);
        }
        return MlsClient.__wrap(ret[0]);
    }
    /**
     * Joins the group a Welcome invites this device into; throws `rejected` when the welcome
     * is for other devices, is for a group other than `expectedGroupId` (when given), or for
     * a group this device is in already. A refused welcome leaves nothing behind.
     * @param {Uint8Array} welcome
     * @param {Uint8Array | null} [expected_group_id]
     * @returns {MlsGroup}
     */
    joinGroup(welcome, expected_group_id) {
        const ptr0 = passArray8ToWasm0(welcome, wasm.__wbindgen_malloc);
        const len0 = WASM_VECTOR_LEN;
        var ptr1 = isLikeNone(expected_group_id) ? 0 : passArray8ToWasm0(expected_group_id, wasm.__wbindgen_malloc);
        var len1 = WASM_VECTOR_LEN;
        const ret = wasm.mlsclient_joinGroup(this.__wbg_ptr, ptr0, len0, ptr1, len1);
        if (ret[2]) {
            throw takeFromExternrefTable0(ret[1]);
        }
        return MlsGroup.__wrap(ret[0]);
    }
    /**
     * A fresh single-use KeyPackage as an MLSMessage: post it to the room for a member to add.
     * @returns {Uint8Array}
     */
    keyPackage() {
        const ret = wasm.mlsclient_keyPackage(this.__wbg_ptr);
        if (ret[3]) {
            throw takeFromExternrefTable0(ret[2]);
        }
        var v1 = getArrayU8FromWasm0(ret[0], ret[1]).slice();
        wasm.__wbindgen_free(ret[0], ret[1] * 1, 1);
        return v1;
    }
    /**
     * A group this device is in, from its state; throws `not_a_member` when there is none.
     * @param {Uint8Array} group_id
     * @returns {MlsGroup}
     */
    loadGroup(group_id) {
        const ptr0 = passArray8ToWasm0(group_id, wasm.__wbindgen_malloc);
        const len0 = WASM_VECTOR_LEN;
        const ret = wasm.mlsclient_loadGroup(this.__wbg_ptr, ptr0, len0);
        if (ret[2]) {
            throw takeFromExternrefTable0(ret[1]);
        }
        return MlsGroup.__wrap(ret[0]);
    }
    /**
     * A new device whose basic credential carries `identity` (1 to 64 bytes: its device id).
     * @param {Uint8Array} identity
     */
    constructor(identity) {
        const ptr0 = passArray8ToWasm0(identity, wasm.__wbindgen_malloc);
        const len0 = WASM_VECTOR_LEN;
        const ret = wasm.mlsclient_new(ptr0, len0);
        if (ret[2]) {
            throw takeFromExternrefTable0(ret[1]);
        }
        this.__wbg_ptr = ret[0];
        MlsClientFinalization.register(this, this.__wbg_ptr, this);
        return this;
    }
    /**
     * @param {Uint8Array} data
     */
    set appData(data) {
        const ptr0 = passArray8ToWasm0(data, wasm.__wbindgen_malloc);
        const len0 = WASM_VECTOR_LEN;
        wasm.mlsclient_set_appData(this.__wbg_ptr, ptr0, len0);
    }
}
if (Symbol.dispose) MlsClient.prototype[Symbol.dispose] = MlsClient.prototype.free;

/**
 * One device's view of one group. Its state is in the client's store as it changes, so the
 * client's `exportState` covers it.
 */
export class MlsGroup {
    static __wrap(ptr) {
        const obj = Object.create(MlsGroup.prototype);
        obj.__wbg_ptr = ptr;
        MlsGroupFinalization.register(obj, obj.__wbg_ptr, obj);
        return obj;
    }
    __destroy_into_raw() {
        const ptr = this.__wbg_ptr;
        this.__wbg_ptr = 0;
        MlsGroupFinalization.unregister(this);
        return ptr;
    }
    free() {
        const ptr = this.__destroy_into_raw();
        wasm.__wbg_mlsgroup_free(ptr, 0);
    }
    /**
     * Whether the group can still be used: false once a commit removed this device.
     * @returns {boolean}
     */
    get active() {
        const ret = wasm.mlsgroup_active(this.__wbg_ptr);
        return ret !== 0;
    }
    /**
     * Commits adding the devices whose key packages are given (an array of Uint8Array).
     * The commit stays pending until `mergePendingCommit` or `clearPendingCommit`.
     * @param {Uint8Array[]} key_packages
     * @returns {AddResult}
     */
    add(key_packages) {
        const ptr0 = passArrayJsValueToWasm0(key_packages, wasm.__wbindgen_malloc);
        const len0 = WASM_VECTOR_LEN;
        const ret = wasm.mlsgroup_add(this.__wbg_ptr, ptr0, len0);
        if (ret[2]) {
            throw takeFromExternrefTable0(ret[1]);
        }
        return AddResult.__wrap(ret[0]);
    }
    clearPendingCommit() {
        const ret = wasm.mlsgroup_clearPendingCommit(this.__wbg_ptr);
        if (ret[1]) {
            throw takeFromExternrefTable0(ret[0]);
        }
    }
    /**
     * An application message (a PrivateMessage) for the current epoch.
     * @param {Uint8Array} plaintext
     * @returns {Uint8Array}
     */
    encrypt(plaintext) {
        const ptr0 = passArray8ToWasm0(plaintext, wasm.__wbindgen_malloc);
        const len0 = WASM_VECTOR_LEN;
        const ret = wasm.mlsgroup_encrypt(this.__wbg_ptr, ptr0, len0);
        if (ret[3]) {
            throw takeFromExternrefTable0(ret[2]);
        }
        var v2 = getArrayU8FromWasm0(ret[0], ret[1]).slice();
        wasm.__wbindgen_free(ret[0], ret[1] * 1, 1);
        return v2;
    }
    /**
     * @returns {number}
     */
    get epoch() {
        const ret = wasm.mlsgroup_epoch(this.__wbg_ptr);
        return ret;
    }
    /**
     * Deletes the group's state from the client's store; every handle to it is unusable
     * afterwards.
     */
    forget() {
        const ptr = this.__destroy_into_raw();
        const ret = wasm.mlsgroup_forget(ptr);
        if (ret[1]) {
            throw takeFromExternrefTable0(ret[0]);
        }
    }
    /**
     * @returns {Uint8Array}
     */
    get groupId() {
        const ret = wasm.mlsgroup_groupId(this.__wbg_ptr);
        var v1 = getArrayU8FromWasm0(ret[0], ret[1]).slice();
        wasm.__wbindgen_free(ret[0], ret[1] * 1, 1);
        return v1;
    }
    /**
     * @returns {boolean}
     */
    get hasPendingCommit() {
        const ret = wasm.mlsgroup_hasPendingCommit(this.__wbg_ptr);
        return ret !== 0;
    }
    /**
     * @returns {number}
     */
    get memberCount() {
        const ret = wasm.mlsgroup_memberCount(this.__wbg_ptr);
        return ret >>> 0;
    }
    /**
     * The members, in leaf order: `[{leaf, identity, fingerprint}]`.
     * @returns {Member[]}
     */
    members() {
        const ret = wasm.mlsgroup_members(this.__wbg_ptr);
        if (ret[3]) {
            throw takeFromExternrefTable0(ret[2]);
        }
        var v1 = getArrayJsValueFromWasm0(ret[0], ret[1]);
        wasm.__wbindgen_free(ret[0], ret[1] * 4, 4);
        return v1;
    }
    mergePendingCommit() {
        const ret = wasm.mlsgroup_mergePendingCommit(this.__wbg_ptr);
        if (ret[1]) {
            throw takeFromExternrefTable0(ret[0]);
        }
    }
    /**
     * Another member's message: decrypts an application message, merges a commit, queues a
     * proposal. Never pass this device's own messages back in.
     * @param {Uint8Array} message
     * @returns {Received}
     */
    process(message) {
        const ptr0 = passArray8ToWasm0(message, wasm.__wbindgen_malloc);
        const len0 = WASM_VECTOR_LEN;
        const ret = wasm.mlsgroup_process(this.__wbg_ptr, ptr0, len0);
        if (ret[2]) {
            throw takeFromExternrefTable0(ret[1]);
        }
        return Received.__wrap(ret[0]);
    }
    /**
     * Commits removing the member whose credential carries `identity`. Pending, as for `add`.
     * @param {Uint8Array} identity
     * @returns {Uint8Array}
     */
    remove(identity) {
        const ptr0 = passArray8ToWasm0(identity, wasm.__wbindgen_malloc);
        const len0 = WASM_VECTOR_LEN;
        const ret = wasm.mlsgroup_remove(this.__wbg_ptr, ptr0, len0);
        if (ret[3]) {
            throw takeFromExternrefTable0(ret[2]);
        }
        var v2 = getArrayU8FromWasm0(ret[0], ret[1]).slice();
        wasm.__wbindgen_free(ret[0], ret[1] * 1, 1);
        return v2;
    }
}
if (Symbol.dispose) MlsGroup.prototype[Symbol.dispose] = MlsGroup.prototype.free;

/**
 * What `MlsGroup.process` found.
 */
export class Received {
    static __wrap(ptr) {
        const obj = Object.create(Received.prototype);
        obj.__wbg_ptr = ptr;
        ReceivedFinalization.register(obj, obj.__wbg_ptr, obj);
        return obj;
    }
    __destroy_into_raw() {
        const ptr = this.__wbg_ptr;
        this.__wbg_ptr = 0;
        ReceivedFinalization.unregister(this);
        return ptr;
    }
    free() {
        const ptr = this.__destroy_into_raw();
        wasm.__wbg_received_free(ptr, 0);
    }
    /**
     * `application`, `commit` (merged: the group is at the next epoch) or `proposal`.
     * @returns {string}
     */
    get kind() {
        let deferred1_0;
        let deferred1_1;
        try {
            const ret = wasm.received_kind(this.__wbg_ptr);
            deferred1_0 = ret[0];
            deferred1_1 = ret[1];
            return getStringFromWasm0(ret[0], ret[1]);
        } finally {
            wasm.__wbindgen_free(deferred1_0, deferred1_1, 1);
        }
    }
    /**
     * An application message's plaintext; empty for the other kinds.
     * @returns {Uint8Array}
     */
    get plaintext() {
        const ret = wasm.received_plaintext(this.__wbg_ptr);
        var v1 = getArrayU8FromWasm0(ret[0], ret[1]).slice();
        wasm.__wbindgen_free(ret[0], ret[1] * 1, 1);
        return v1;
    }
    /**
     * The commit removed this device; the group can no longer be used.
     * @returns {boolean}
     */
    get selfRemoved() {
        const ret = wasm.received_selfRemoved(this.__wbg_ptr);
        return ret !== 0;
    }
    /**
     * The sender's leaf index.
     * @returns {number | undefined}
     */
    get senderLeaf() {
        const ret = wasm.received_senderLeaf(this.__wbg_ptr);
        return ret === Number.MAX_SAFE_INTEGER ? undefined : ret;
    }
    /**
     * The identity in the sender's credential: for a commit, the committer's.
     * @returns {Uint8Array}
     */
    get sender() {
        const ret = wasm.received_sender(this.__wbg_ptr);
        var v1 = getArrayU8FromWasm0(ret[0], ret[1]).slice();
        wasm.__wbindgen_free(ret[0], ret[1] * 1, 1);
        return v1;
    }
}
if (Symbol.dispose) Received.prototype[Symbol.dispose] = Received.prototype.free;

/**
 * The ciphersuite every group and key package uses, by its RFC 9420 code point (1).
 * @returns {number}
 */
export function ciphersuite() {
    const ret = wasm.ciphersuite();
    return ret;
}

/**
 * Reads what kind of MLSMessage a body is. Throws `malformed` for anything else, and
 * `rejected` for a key package whose signature or ciphersuite is wrong.
 * @param {Uint8Array} bytes
 * @returns {MessageInfo}
 */
export function inspect(bytes) {
    const ptr0 = passArray8ToWasm0(bytes, wasm.__wbindgen_malloc);
    const len0 = WASM_VECTOR_LEN;
    const ret = wasm.inspect(ptr0, len0);
    if (ret[2]) {
        throw takeFromExternrefTable0(ret[1]);
    }
    return MessageInfo.__wrap(ret[0]);
}
function __wbg_get_imports() {
    const import0 = {
        __proto__: null,
        __wbg_Error_30c8987f7c2ed4e2: function(arg0, arg1) {
            const ret = Error(getStringFromWasm0(arg0, arg1));
            return ret;
        },
        __wbg___wbindgen_is_function_1f9d30630b8b1d3d: function(arg0) {
            const ret = typeof(arg0) === 'function';
            return ret;
        },
        __wbg___wbindgen_is_object_3c45d4f2dde4e749: function(arg0) {
            const val = arg0;
            const ret = typeof(val) === 'object' && val !== null;
            return ret;
        },
        __wbg___wbindgen_is_string_90b56bc79aad6f6c: function(arg0) {
            const ret = typeof(arg0) === 'string';
            return ret;
        },
        __wbg___wbindgen_is_undefined_8865fb403f8fe9d8: function(arg0) {
            const ret = arg0 === undefined;
            return ret;
        },
        __wbg___wbindgen_throw_41e9ee4f547fc59a: function(arg0, arg1) {
            throw new Error(getStringFromWasm0(arg0, arg1));
        },
        __wbg_call_187d372bd5fdd4aa: function() { return handleError(function (arg0, arg1, arg2) {
            const ret = arg0.call(arg1, arg2);
            return ret;
        }, arguments); },
        __wbg_crypto_38df2bab126b63dc: function(arg0) {
            const ret = arg0.crypto;
            return ret;
        },
        __wbg_getRandomValues_436a51d0629d84e1: function() { return handleError(function (arg0, arg1) {
            globalThis.crypto.getRandomValues(getArrayU8FromWasm0(arg0, arg1));
        }, arguments); },
        __wbg_getRandomValues_c44a50d8cfdaebeb: function() { return handleError(function (arg0, arg1) {
            arg0.getRandomValues(arg1);
        }, arguments); },
        __wbg_length_7f3c00c40364105e: function(arg0) {
            const ret = arg0.length;
            return ret;
        },
        __wbg_member_new: function(arg0) {
            const ret = Member.__wrap(arg0);
            return ret;
        },
        __wbg_msCrypto_bd5a034af96bcba6: function(arg0) {
            const ret = arg0.msCrypto;
            return ret;
        },
        __wbg_new_with_length_3da0ad195f6f63ba: function(arg0) {
            const ret = new Uint8Array(arg0 >>> 0);
            return ret;
        },
        __wbg_node_84ea875411254db1: function(arg0) {
            const ret = arg0.node;
            return ret;
        },
        __wbg_now_aa4ccb83129e9e55: function() {
            const ret = Date.now();
            return ret;
        },
        __wbg_process_44c7a14e11e9f69e: function(arg0) {
            const ret = arg0.process;
            return ret;
        },
        __wbg_prototypesetcall_bc27214492979395: function(arg0, arg1, arg2) {
            Uint8Array.prototype.set.call(getArrayU8FromWasm0(arg0, arg1), arg2);
        },
        __wbg_randomFillSync_6c25eac9869eb53c: function() { return handleError(function (arg0, arg1) {
            arg0.randomFillSync(arg1);
        }, arguments); },
        __wbg_require_b4edbdcf3e2a1ef0: function() { return handleError(function () {
            const ret = module.require;
            return ret;
        }, arguments); },
        __wbg_static_accessor_GLOBAL_266715b9d96ba635: function() {
            const ret = typeof global === 'undefined' ? null : global;
            return isLikeNone(ret) ? 0 : addToExternrefTable0(ret);
        },
        __wbg_static_accessor_GLOBAL_THIS_10fb7dc1ae063179: function() {
            const ret = typeof globalThis === 'undefined' ? null : globalThis;
            return isLikeNone(ret) ? 0 : addToExternrefTable0(ret);
        },
        __wbg_static_accessor_SELF_0b583911f537483a: function() {
            const ret = typeof self === 'undefined' ? null : self;
            return isLikeNone(ret) ? 0 : addToExternrefTable0(ret);
        },
        __wbg_static_accessor_WINDOW_d7f903d1508cbdc4: function() {
            const ret = typeof window === 'undefined' ? null : window;
            return isLikeNone(ret) ? 0 : addToExternrefTable0(ret);
        },
        __wbg_subarray_002b94d5e13d1411: function(arg0, arg1, arg2) {
            const ret = arg0.subarray(arg1 >>> 0, arg2 >>> 0);
            return ret;
        },
        __wbg_versions_276b2795b1c6a219: function(arg0) {
            const ret = arg0.versions;
            return ret;
        },
        __wbindgen_generic_0000000000000001: function(arg0, arg1) {
            // Cast intrinsic for `Ref(Slice(U8)) -> NamedExternref("Uint8Array")`.
            const ret = getArrayU8FromWasm0(arg0, arg1);
            return ret;
        },
        __wbindgen_generic_0000000000000002: function(arg0, arg1) {
            // Cast intrinsic for `Ref(String) -> Externref`.
            const ret = getStringFromWasm0(arg0, arg1);
            return ret;
        },
        __wbindgen_init_externref_table: function() {
            const table = wasm.__wbindgen_externrefs;
            const offset = table.grow(4);
            table.set(0, undefined);
            table.set(offset + 0, undefined);
            table.set(offset + 1, null);
            table.set(offset + 2, true);
            table.set(offset + 3, false);
        },
    };
    return {
        __proto__: null,
        "./web_mls_bg.js": import0,
    };
}

const AddResultFinalization = (typeof FinalizationRegistry === 'undefined')
    ? { register: () => {}, unregister: () => {} }
    : new FinalizationRegistry(ptr => wasm.__wbg_addresult_free(ptr, 1));
const MemberFinalization = (typeof FinalizationRegistry === 'undefined')
    ? { register: () => {}, unregister: () => {} }
    : new FinalizationRegistry(ptr => wasm.__wbg_member_free(ptr, 1));
const MessageInfoFinalization = (typeof FinalizationRegistry === 'undefined')
    ? { register: () => {}, unregister: () => {} }
    : new FinalizationRegistry(ptr => wasm.__wbg_messageinfo_free(ptr, 1));
const MlsClientFinalization = (typeof FinalizationRegistry === 'undefined')
    ? { register: () => {}, unregister: () => {} }
    : new FinalizationRegistry(ptr => wasm.__wbg_mlsclient_free(ptr, 1));
const MlsGroupFinalization = (typeof FinalizationRegistry === 'undefined')
    ? { register: () => {}, unregister: () => {} }
    : new FinalizationRegistry(ptr => wasm.__wbg_mlsgroup_free(ptr, 1));
const ReceivedFinalization = (typeof FinalizationRegistry === 'undefined')
    ? { register: () => {}, unregister: () => {} }
    : new FinalizationRegistry(ptr => wasm.__wbg_received_free(ptr, 1));

function addToExternrefTable0(obj) {
    const idx = wasm.__externref_table_alloc();
    wasm.__wbindgen_externrefs.set(idx, obj);
    return idx;
}

function getArrayJsValueFromWasm0(ptr, len) {
    ptr = ptr >>> 0;
    const mem = getDataViewMemory0();
    const result = [];
    for (let i = ptr; i < ptr + 4 * len; i += 4) {
        result.push(wasm.__wbindgen_externrefs.get(mem.getUint32(i, true)));
    }
    wasm.__externref_drop_slice(ptr, len);
    return result;
}

function getArrayU8FromWasm0(ptr, len) {
    ptr = ptr >>> 0;
    return getUint8ArrayMemory0().subarray(ptr / 1, ptr / 1 + len);
}

let cachedDataViewMemory0 = null;
function getDataViewMemory0() {
    if (cachedDataViewMemory0 === null || cachedDataViewMemory0.buffer.detached === true || (cachedDataViewMemory0.buffer.detached === undefined && cachedDataViewMemory0.buffer !== wasm.memory.buffer)) {
        cachedDataViewMemory0 = new DataView(wasm.memory.buffer);
    }
    return cachedDataViewMemory0;
}

function getStringFromWasm0(ptr, len) {
    return decodeText(ptr >>> 0, len);
}

let cachedUint8ArrayMemory0 = null;
function getUint8ArrayMemory0() {
    if (cachedUint8ArrayMemory0 === null || cachedUint8ArrayMemory0.byteLength === 0) {
        cachedUint8ArrayMemory0 = new Uint8Array(wasm.memory.buffer);
    }
    return cachedUint8ArrayMemory0;
}

function handleError(f, args) {
    try {
        return f.apply(this, args);
    } catch (e) {
        const idx = addToExternrefTable0(e);
        wasm.__wbindgen_exn_store(idx);
    }
}

function isLikeNone(x) {
    return x === undefined || x === null;
}

function passArray8ToWasm0(arg, malloc) {
    const ptr = malloc(arg.length * 1, 1) >>> 0;
    getUint8ArrayMemory0().set(arg, ptr / 1);
    WASM_VECTOR_LEN = arg.length;
    return ptr;
}

function passArrayJsValueToWasm0(array, malloc) {
    const ptr = malloc(array.length * 4, 4) >>> 0;
    for (let i = 0; i < array.length; i++) {
        const add = addToExternrefTable0(array[i]);
        getDataViewMemory0().setUint32(ptr + 4 * i, add, true);
    }
    WASM_VECTOR_LEN = array.length;
    return ptr;
}

function takeFromExternrefTable0(idx) {
    const value = wasm.__wbindgen_externrefs.get(idx);
    wasm.__externref_table_dealloc(idx);
    return value;
}

let cachedTextDecoder = new TextDecoder('utf-8', { ignoreBOM: true, fatal: true });
cachedTextDecoder.decode();
const MAX_SAFARI_DECODE_BYTES = 2146435072;
let numBytesDecoded = 0;
function decodeText(ptr, len) {
    numBytesDecoded += len;
    if (numBytesDecoded >= MAX_SAFARI_DECODE_BYTES) {
        cachedTextDecoder = new TextDecoder('utf-8', { ignoreBOM: true, fatal: true });
        cachedTextDecoder.decode();
        numBytesDecoded = len;
    }
    return cachedTextDecoder.decode(getUint8ArrayMemory0().subarray(ptr, ptr + len));
}

let WASM_VECTOR_LEN = 0;

let wasmModule, wasmInstance, wasm;
function __wbg_finalize_init(instance, module) {
    wasmInstance = instance;
    wasm = instance.exports;
    wasmModule = module;
    cachedDataViewMemory0 = null;
    cachedUint8ArrayMemory0 = null;
    wasm.__wbindgen_start();
    return wasm;
}

async function __wbg_load(module, imports) {
    if (typeof Response === 'function' && module instanceof Response) {
        if (!module.ok) {
            throw new Error(`failed to fetch Wasm: ${module.status} ${module.statusText} fetching '${module.url}'`);
        }

        if (typeof WebAssembly.instantiateStreaming === 'function') {
            try {
                return await WebAssembly.instantiateStreaming(module, imports);
            } catch (e) {
                const validResponse = expectedResponseType(module.type);

                if (validResponse && module.headers.get('Content-Type') !== 'application/wasm') {
                    console.warn("`WebAssembly.instantiateStreaming` failed because your server does not serve Wasm with `application/wasm` MIME type. Falling back to `WebAssembly.instantiate` which is slower. Original error:\n", e);

                } else { throw e; }
            }
        }

        const bytes = await module.arrayBuffer();
        return await WebAssembly.instantiate(bytes, imports);
    } else {
        const instance = await WebAssembly.instantiate(module, imports);

        if (instance instanceof WebAssembly.Instance) {
            return { instance, module };
        } else {
            return instance;
        }
    }

    function expectedResponseType(type) {
        switch (type) {
            case 'basic': case 'cors': case 'default': return true;
        }
        return false;
    }
}

function initSync(module) {
    if (wasm !== undefined) return wasm;


    if (module !== undefined) {
        if (Object.getPrototypeOf(module) === Object.prototype) {
            ({module} = module)
        } else {
            console.warn('using deprecated parameters for `initSync()`; pass a single object instead')
        }
    }

    const imports = __wbg_get_imports();
    if (!(module instanceof WebAssembly.Module)) {
        module = new WebAssembly.Module(module);
    }
    const instance = new WebAssembly.Instance(module, imports);
    return __wbg_finalize_init(instance, module);
}

async function __wbg_init(module_or_path) {
    if (wasm !== undefined) return wasm;


    if (module_or_path !== undefined) {
        if (Object.getPrototypeOf(module_or_path) === Object.prototype) {
            ({module_or_path} = module_or_path)
        } else {
            console.warn('using deprecated parameters for the initialization function; pass a single object instead')
        }
    }

    if (module_or_path === undefined) {
        module_or_path = new URL('web_mls_bg.wasm', import.meta.url);
    }
    const imports = __wbg_get_imports();

    if (typeof module_or_path === 'string' || (typeof Request === 'function' && module_or_path instanceof Request) || (typeof URL === 'function' && module_or_path instanceof URL)) {
        module_or_path = fetch(module_or_path);
    }

    const { instance, module } = await __wbg_load(await module_or_path, imports);

    return __wbg_finalize_init(instance, module);
}

export { initSync, __wbg_init as default };
