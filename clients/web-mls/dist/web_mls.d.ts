/* tslint:disable */
/* eslint-disable */

/**
 * What `MlsGroup.add` made.
 */
export class AddResult {
    private constructor();
    free(): void;
    [Symbol.dispose](): void;
    /**
     * For the room first. Merge once the room sequenced it, then send the welcome.
     */
    readonly commit: Uint8Array;
    readonly welcome: Uint8Array;
}

/**
 * What a room message's body is, read without keys.
 */
export class MessageInfo {
    private constructor();
    free(): void;
    [Symbol.dispose](): void;
    /**
     * A public or private message's `application`, `proposal` or `commit`.
     */
    readonly contentType: string | undefined;
    /**
     * A public or private message's epoch (exact below 2^53).
     */
    readonly epoch: number | undefined;
    /**
     * A public or private message's group id.
     */
    readonly groupId: Uint8Array | undefined;
    /**
     * A key package's identity; its signature and ciphersuite have been checked.
     */
    readonly identity: Uint8Array | undefined;
    /**
     * `key_package`, `welcome`, `private_message`, `public_message` or `group_info`.
     */
    readonly wireFormat: string;
}

/**
 * A device: its identity, signature key, key packages and groups.
 */
export class MlsClient {
    free(): void;
    [Symbol.dispose](): void;
    /**
     * A new group, at epoch 0, with this device its only member. The chat convention is the
     * room id's text (36 bytes) as the group id.
     */
    createGroup(group_id: Uint8Array): MlsGroup;
    /**
     * Everything this device holds, as bytes for IndexedDB. Secret: whoever has them is this
     * device. Export again after every call that changes a group or makes a key package.
     */
    exportState(): Uint8Array;
    /**
     * The device `exportState` saved, groups and unused key packages included.
     */
    static importState(state: Uint8Array): MlsClient;
    /**
     * Joins the group a Welcome invites this device into; throws `rejected` when the welcome
     * is for other devices.
     */
    joinGroup(welcome: Uint8Array): MlsGroup;
    /**
     * A fresh single-use KeyPackage as an MLSMessage: post it to the room for a member to add.
     */
    keyPackage(): Uint8Array;
    /**
     * A group this device is in, from its state; throws `not_a_member` when there is none.
     */
    loadGroup(group_id: Uint8Array): MlsGroup;
    /**
     * A new device whose basic credential carries `identity` (1 to 64 bytes: its device id).
     */
    constructor(identity: Uint8Array);
    readonly identity: Uint8Array;
}

/**
 * One device's view of one group. Its state is in the client's store as it changes, so the
 * client's `exportState` covers it.
 */
export class MlsGroup {
    private constructor();
    free(): void;
    [Symbol.dispose](): void;
    /**
     * Commits adding the devices whose key packages are given (an array of Uint8Array).
     * The commit stays pending until `mergePendingCommit` or `clearPendingCommit`.
     */
    add(key_packages: Uint8Array[]): AddResult;
    clearPendingCommit(): void;
    /**
     * An application message (a PrivateMessage) for the current epoch.
     */
    encrypt(plaintext: Uint8Array): Uint8Array;
    /**
     * Deletes the group's state from the client's store; the handle is unusable afterwards.
     */
    forget(): void;
    /**
     * Members' identities, in leaf order.
     */
    members(): Uint8Array[];
    mergePendingCommit(): void;
    /**
     * Another member's message: decrypts an application message, merges a commit, queues a
     * proposal. Never pass this device's own messages back in.
     */
    process(message: Uint8Array): Received;
    /**
     * Commits removing the member whose credential carries `identity`. Pending, as for `add`.
     */
    remove(identity: Uint8Array): Uint8Array;
    readonly epoch: number;
    readonly groupId: Uint8Array;
    readonly hasPendingCommit: boolean;
    readonly memberCount: number;
}

/**
 * What `MlsGroup.process` found.
 */
export class Received {
    private constructor();
    free(): void;
    [Symbol.dispose](): void;
    /**
     * `application`, `commit` (merged: the group is at the next epoch) or `proposal`.
     */
    readonly kind: string;
    /**
     * An application message's plaintext; empty for the other kinds.
     */
    readonly plaintext: Uint8Array;
    /**
     * The identity in the sender's credential.
     */
    readonly sender: Uint8Array;
}

/**
 * The ciphersuite every group and key package uses, by its RFC 9420 code point (1).
 */
export function ciphersuite(): number;

/**
 * Reads what kind of MLSMessage a body is. Throws `malformed` for anything else, and
 * `rejected` for a key package whose signature or ciphersuite is wrong.
 */
export function inspect(bytes: Uint8Array): MessageInfo;

export type InitInput = RequestInfo | URL | Response | BufferSource | WebAssembly.Module;

export interface InitOutput {
    readonly memory: WebAssembly.Memory;
    readonly __wbg_addresult_free: (a: number, b: number) => void;
    readonly __wbg_messageinfo_free: (a: number, b: number) => void;
    readonly __wbg_mlsclient_free: (a: number, b: number) => void;
    readonly __wbg_mlsgroup_free: (a: number, b: number) => void;
    readonly __wbg_received_free: (a: number, b: number) => void;
    readonly addresult_commit: (a: number) => [number, number];
    readonly addresult_welcome: (a: number) => [number, number];
    readonly ciphersuite: () => number;
    readonly inspect: (a: number, b: number) => [number, number, number];
    readonly messageinfo_contentType: (a: number) => [number, number];
    readonly messageinfo_epoch: (a: number) => [number, number];
    readonly messageinfo_groupId: (a: number) => [number, number];
    readonly messageinfo_identity: (a: number) => [number, number];
    readonly messageinfo_wireFormat: (a: number) => [number, number];
    readonly mlsclient_createGroup: (a: number, b: number, c: number) => [number, number, number];
    readonly mlsclient_exportState: (a: number) => [number, number, number, number];
    readonly mlsclient_identity: (a: number) => [number, number];
    readonly mlsclient_importState: (a: number, b: number) => [number, number, number];
    readonly mlsclient_joinGroup: (a: number, b: number, c: number) => [number, number, number];
    readonly mlsclient_keyPackage: (a: number) => [number, number, number, number];
    readonly mlsclient_loadGroup: (a: number, b: number, c: number) => [number, number, number];
    readonly mlsclient_new: (a: number, b: number) => [number, number, number];
    readonly mlsgroup_add: (a: number, b: number, c: number) => [number, number, number];
    readonly mlsgroup_clearPendingCommit: (a: number) => [number, number];
    readonly mlsgroup_encrypt: (a: number, b: number, c: number) => [number, number, number, number];
    readonly mlsgroup_epoch: (a: number) => number;
    readonly mlsgroup_forget: (a: number) => [number, number];
    readonly mlsgroup_groupId: (a: number) => [number, number];
    readonly mlsgroup_hasPendingCommit: (a: number) => number;
    readonly mlsgroup_memberCount: (a: number) => number;
    readonly mlsgroup_members: (a: number) => [number, number];
    readonly mlsgroup_mergePendingCommit: (a: number) => [number, number];
    readonly mlsgroup_process: (a: number, b: number, c: number) => [number, number, number];
    readonly mlsgroup_remove: (a: number, b: number, c: number) => [number, number, number, number];
    readonly received_kind: (a: number) => [number, number];
    readonly received_plaintext: (a: number) => [number, number];
    readonly received_sender: (a: number) => [number, number];
    readonly __wbindgen_exn_store: (a: number) => void;
    readonly __externref_table_alloc: () => number;
    readonly __wbindgen_externrefs: WebAssembly.Table;
    readonly __wbindgen_free: (a: number, b: number, c: number) => void;
    readonly __wbindgen_malloc: (a: number, b: number) => number;
    readonly __externref_table_dealloc: (a: number) => void;
    readonly __externref_drop_slice: (a: number, b: number) => void;
    readonly __wbindgen_start: () => void;
}

export type SyncInitInput = BufferSource | WebAssembly.Module;

/**
 * Instantiates the given `module`, which can either be bytes or
 * a precompiled `WebAssembly.Module`.
 *
 * @param {{ module: SyncInitInput }} module - Passing `SyncInitInput` directly is deprecated.
 *
 * @returns {InitOutput}
 */
export function initSync(module: { module: SyncInitInput } | SyncInitInput): InitOutput;

/**
 * If `module_or_path` is {RequestInfo} or {URL}, makes a request and
 * for everything else, calls `WebAssembly.instantiate` directly.
 *
 * @param {{ module_or_path: InitInput | Promise<InitInput> }} module_or_path - Passing `InitInput` directly is deprecated.
 *
 * @returns {Promise<InitOutput>}
 */
export default function __wbg_init (module_or_path?: { module_or_path: InitInput | Promise<InitInput> } | InitInput | Promise<InitInput>): Promise<InitOutput>;
