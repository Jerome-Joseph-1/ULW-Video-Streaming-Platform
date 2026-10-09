// The client's core, natively: what the WebAssembly build runs, minus the bindings. The
// cross-implementation check, the browser build against the FFI bridge through a chat server,
// is interop/run.sh.

use web_mls::core::{Client, Error, Kind, inspect, members};

const ROOM: &[u8] = b"0192f0c4-8a1e-7c3a-9d2b-5f6e7a8b9c0d";

fn info_fingerprint(client: &Client) -> String {
    client.fingerprint().unwrap()
}

#[test]
fn two_devices_form_a_group_and_talk() {
    let alice = Client::new(b"alice").unwrap();
    let bob = Client::new(b"bob").unwrap();
    let mut group = alice.create_group(ROOM).unwrap();

    let package = bob.key_package().unwrap();
    let info = inspect(&package).unwrap();
    assert_eq!(info.wire_format, "key_package");
    assert_eq!(info.identity.as_deref(), Some(&b"bob"[..]));
    assert_eq!(info.fingerprint, Some(bob.fingerprint().unwrap()));
    assert_eq!(info.key_package_ref.as_ref().map(String::len), Some(64));
    assert_eq!(
        inspect(&package).unwrap().key_package_ref,
        info.key_package_ref
    );

    let (commit, welcome) = alice.add(&mut group, &[&package]).unwrap();
    // OpenMLS's default policy, as the bridge's: handshake messages are encrypted too.
    assert_eq!(inspect(&commit).unwrap().wire_format, "private_message");
    assert_eq!(inspect(&commit).unwrap().content_type, Some("commit"));
    assert_eq!(inspect(&commit).unwrap().epoch, Some(0));
    assert_eq!(inspect(&welcome).unwrap().wire_format, "welcome");
    alice.merge_pending_commit(&mut group).unwrap();

    // A welcome for someone else is refused, and changes nothing.
    let carol = Client::new(b"carol").unwrap();
    assert_eq!(carol.join(&welcome, None).err(), Some(Error::Rejected));

    let mut bobs = bob.join(&welcome, None).unwrap();
    let seen = members(&bobs).unwrap();
    let names: Vec<_> = seen.iter().map(|m| m.identity.clone()).collect();
    assert_eq!(names, vec![b"alice".to_vec(), b"bob".to_vec()]);
    assert_eq!(seen[0].fingerprint, alice.fingerprint().unwrap());
    assert_eq!(seen[1].fingerprint, info_fingerprint(&bob));
    assert_eq!(seen[0].leaf, 0);
    assert_eq!(bobs.epoch().as_u64(), 1);

    let sealed = alice.encrypt(&mut group, b"hello bob").unwrap();
    let info = inspect(&sealed).unwrap();
    assert_eq!(info.wire_format, "private_message");
    assert_eq!(info.group_id.as_deref(), Some(ROOM));
    assert_eq!(info.content_type, Some("application"));
    let read = bob.process(&mut bobs, &sealed).unwrap();
    assert_eq!(read.kind, Kind::Application);
    assert_eq!(read.plaintext, b"hello bob");
    assert_eq!(read.sender, b"alice");
    // RFC 9420 deletes a key once used: the same message twice is refused.
    assert_eq!(bob.process(&mut bobs, &sealed).err(), Some(Error::Rejected));
}

#[test]
fn exported_state_restores_the_device_its_key_packages_and_groups() {
    let alice = Client::new(b"alice").unwrap();
    let bob = Client::new(b"bob").unwrap();
    let mut group = alice.create_group(ROOM).unwrap();
    // Bob's package goes out before he saves; the welcome arrives after he restores.
    let package = bob.key_package().unwrap();
    let saved = bob.export_state().unwrap();
    assert_eq!(
        saved,
        bob.export_state().unwrap(),
        "the same state, the same bytes"
    );
    drop(bob);

    let (_, welcome) = alice.add(&mut group, &[&package]).unwrap();
    alice.merge_pending_commit(&mut group).unwrap();
    let bob = Client::import_state(&saved).unwrap();
    assert_eq!(bob.identity(), b"bob");
    let mut bobs = bob.join(&welcome, None).unwrap();
    let sealed = bob.encrypt(&mut bobs, b"back again").unwrap();
    assert_eq!(
        alice.process(&mut group, &sealed).unwrap().plaintext,
        b"back again"
    );

    // And a group, once joined, comes back from the next export.
    let saved = bob.export_state().unwrap();
    drop(bobs);
    drop(bob);
    let bob = Client::import_state(&saved).unwrap();
    let mut bobs = bob.load_group(ROOM).unwrap();
    let sealed = alice.encrypt(&mut group, b"still here").unwrap();
    assert_eq!(
        bob.process(&mut bobs, &sealed).unwrap().plaintext,
        b"still here"
    );
    assert_eq!(bob.load_group(b"another").err(), Some(Error::NotAMember));

    assert_eq!(
        Client::import_state(b"nonsense").err(),
        Some(Error::Malformed)
    );
    assert_eq!(
        Client::import_state(&saved[..saved.len() - 1]).err(),
        Some(Error::Malformed)
    );
}

#[test]
fn a_third_member_is_added_by_the_first_and_only_by_the_first() {
    let alice = Client::new(b"alice").unwrap();
    let bob = Client::new(b"bob").unwrap();
    let carol = Client::new(b"carol").unwrap();
    let mut a = alice.create_group(ROOM).unwrap();
    let (_, welcome) = alice.add(&mut a, &[&bob.key_package().unwrap()]).unwrap();
    alice.merge_pending_commit(&mut a).unwrap();
    let mut b = bob.join(&welcome, Some(ROOM)).unwrap();

    // Bob is not at the first leaf: his add is refused, and alice's group is untouched.
    let (by_bob, _) = bob.add(&mut b, &[&carol.key_package().unwrap()]).unwrap();
    assert_eq!(alice.process(&mut a, &by_bob).err(), Some(Error::Rejected));
    assert_eq!(a.epoch().as_u64(), 1);
    bob.clear_pending_commit(&mut b).unwrap();

    let dave = Client::new(b"dave").unwrap();
    let (commit, welcome) = alice.add(&mut a, &[&dave.key_package().unwrap()]).unwrap();
    alice.merge_pending_commit(&mut a).unwrap();
    let seen = bob.process(&mut b, &commit).unwrap();
    assert_eq!(seen.kind, Kind::Commit);
    assert_eq!(seen.sender, b"alice");
    assert_eq!(seen.sender_leaf, Some(0));
    // A welcome for another group is refused, and leaves nothing behind.
    assert_eq!(
        dave.join(&welcome, Some(b"another")).err(),
        Some(Error::Rejected)
    );
    assert_eq!(dave.load_group(ROOM).err(), Some(Error::NotAMember));
    let (commit, welcome) = alice.add(&mut a, &[&carol.key_package().unwrap()]).unwrap();
    alice.merge_pending_commit(&mut a).unwrap();
    bob.process(&mut b, &commit).unwrap();
    let mut c = carol.join(&welcome, Some(ROOM)).unwrap();
    assert_eq!(a.epoch(), c.epoch());
    // ... and so is a second welcome into a group it is in.
    assert_eq!(
        carol.join(&welcome, Some(ROOM)).err(),
        Some(Error::Rejected)
    );

    let sealed = carol.encrypt(&mut c, b"hi both").unwrap();
    assert_eq!(
        alice.process(&mut a, &sealed).unwrap().plaintext,
        b"hi both"
    );
    assert_eq!(bob.process(&mut b, &sealed).unwrap().plaintext, b"hi both");

    // Bob may remove (only adds are the first leaf's); carol learns she is out.
    let commit = bob.remove(&mut b, b"carol").unwrap();
    bob.merge_pending_commit(&mut b).unwrap();
    assert!(!alice.process(&mut a, &commit).unwrap().self_removed);
    let gone = carol.process(&mut c, &commit).unwrap();
    assert_eq!(gone.kind, Kind::Commit);
    assert!(gone.self_removed);
    let sealed = alice.encrypt(&mut a, b"just us").unwrap();
    assert_eq!(bob.process(&mut b, &sealed).unwrap().plaintext, b"just us");
    assert!(carol.process(&mut c, &sealed).is_err());
    assert_eq!(
        alice.remove(&mut a, b"carol").err(),
        Some(Error::NotAMember)
    );
}

#[test]
fn a_message_from_the_epoch_before_a_commit_still_decrypts() {
    let alice = Client::new(b"alice").unwrap();
    let bob = Client::new(b"bob").unwrap();
    let carol = Client::new(b"carol").unwrap();
    let mut a = alice.create_group(ROOM).unwrap();
    let (_, welcome) = alice.add(&mut a, &[&bob.key_package().unwrap()]).unwrap();
    alice.merge_pending_commit(&mut a).unwrap();
    let mut b = bob.join(&welcome, None).unwrap();
    // Bob writes at epoch 1; the room sequences alice's commit to epoch 2 first.
    let late = bob.encrypt(&mut b, b"sent at epoch 1").unwrap();
    let (commit, _) = alice.add(&mut a, &[&carol.key_package().unwrap()]).unwrap();
    alice.merge_pending_commit(&mut a).unwrap();
    bob.process(&mut b, &commit).unwrap();
    assert_eq!(
        alice.process(&mut a, &late).unwrap().plaintext,
        b"sent at epoch 1"
    );
}

#[test]
fn proposals_are_refused() {
    use openmls::prelude::tls_codec::Deserialize as _;
    use openmls::prelude::{
        BasicCredential, CredentialWithKey, KeyPackage, MlsGroupJoinConfig, MlsMessageBodyIn,
        MlsMessageIn, MlsMessageOut, ProtocolVersion, StagedWelcome,
    };
    use openmls_basic_credential::SignatureKeyPair;
    use openmls_rust_crypto::OpenMlsRustCrypto;
    use openmls_traits::OpenMlsProvider as _;

    // Bob is a plain OpenMLS member, so he can send what this client never does: a proposal.
    let provider = OpenMlsRustCrypto::default();
    let suite = web_mls::core::CIPHERSUITE;
    let signer = SignatureKeyPair::new(suite.signature_algorithm()).unwrap();
    let credential = CredentialWithKey {
        credential: BasicCredential::new(b"bob".to_vec()).into(),
        signature_key: signer.to_public_vec().into(),
    };
    let bundle = KeyPackage::builder()
        .build(suite, &provider, &signer, credential)
        .unwrap();
    let package = MlsMessageOut::from(bundle.key_package().clone())
        .to_bytes()
        .unwrap();

    let alice = Client::new(b"alice").unwrap();
    let mut a = alice.create_group(ROOM).unwrap();
    let (_, welcome) = alice.add(&mut a, &[&package]).unwrap();
    alice.merge_pending_commit(&mut a).unwrap();
    let MlsMessageBodyIn::Welcome(welcome) = MlsMessageIn::tls_deserialize_exact(&welcome)
        .unwrap()
        .extract()
    else {
        panic!("not a welcome")
    };
    let config = MlsGroupJoinConfig::builder()
        .use_ratchet_tree_extension(true)
        .build();
    let mut b = StagedWelcome::new_from_welcome(&provider, &config, welcome, None)
        .unwrap()
        .into_group(&provider)
        .unwrap();

    let mallory = Client::new(b"mallory").unwrap();
    let MlsMessageBodyIn::KeyPackage(theirs) =
        MlsMessageIn::tls_deserialize_exact(mallory.key_package().unwrap())
            .unwrap()
            .extract()
    else {
        panic!("not a key package")
    };
    let theirs = theirs
        .validate(provider.crypto(), ProtocolVersion::Mls10)
        .unwrap();
    let (proposal, _) = b.propose_add_member(&provider, &signer, &theirs).unwrap();
    let proposal = proposal.to_bytes().unwrap();
    assert_eq!(inspect(&proposal).unwrap().content_type, Some("proposal"));
    assert_eq!(
        alice.process(&mut a, &proposal).err(),
        Some(Error::Rejected)
    );

    // Alice's next commit carries nothing of it.
    let carol = Client::new(b"carol").unwrap();
    alice.add(&mut a, &[&carol.key_package().unwrap()]).unwrap();
    alice.merge_pending_commit(&mut a).unwrap();
    let names: Vec<_> = members(&a)
        .unwrap()
        .into_iter()
        .map(|m| m.identity)
        .collect();
    assert_eq!(
        names,
        vec![b"alice".to_vec(), b"bob".to_vec(), b"carol".to_vec()]
    );
}

#[test]
fn bad_input_is_refused_by_name() {
    assert_eq!(Client::new(b"").err(), Some(Error::InvalidArgument));
    assert_eq!(Client::new(&[b'x'; 65]).err(), Some(Error::InvalidArgument));
    assert_eq!(inspect(b"\x00\x01").err(), Some(Error::Malformed));
    let alice = Client::new(b"alice").unwrap();
    let mut group = alice.create_group(ROOM).unwrap();
    assert_eq!(
        alice.add(&mut group, &[]).err(),
        Some(Error::InvalidArgument)
    );
    let sealed = alice.encrypt(&mut group, b"x").unwrap();
    assert_eq!(
        alice.add(&mut group, &[&sealed]).err(),
        Some(Error::Malformed)
    );
    assert_eq!(alice.join(&sealed, None).err(), Some(Error::Malformed));
}

// ADR-0101: the key directory hands a device's last-resort package out again once its single-use
// ones have run out. Two groups that took the same one both welcome the device, and the package
// stays usable across an export.
#[test]
fn a_last_resort_key_package_serves_more_than_one_welcome() {
    let alice = Client::new(b"alice/01a0eb86-6cca-7dce-84cc-3bb47615f9fd").unwrap();
    let carol = Client::new(b"carol/01a0eb86-6cca-7dce-84cc-3bb47615f9fe").unwrap();
    let bob = Client::new(b"bob/01a0eb86-6cca-7dce-84cc-3bb47615f9ff").unwrap();
    let last = bob.last_resort_key_package().unwrap();
    let info = inspect(&last).unwrap();
    assert_eq!(info.wire_format, "key_package");
    assert_eq!(info.last_resort, Some(true));
    assert_eq!(
        inspect(&bob.key_package().unwrap()).unwrap().last_resort,
        Some(false)
    );

    let mut first = alice.create_group(ROOM).unwrap();
    let (_, welcome) = alice.add(&mut first, &[&last]).unwrap();
    alice.merge_pending_commit(&mut first).unwrap();
    let mut bobs_first = bob.join(&welcome, None).unwrap();

    // Saved and restored between the two welcomes, as a page reload would.
    let saved = bob.export_state().unwrap();
    drop(bobs_first);
    drop(bob);
    let bob = Client::import_state(&saved).unwrap();
    bobs_first = bob.load_group(ROOM).unwrap();

    let other: &[u8] = b"0192f0c4-8a1e-7c3a-9d2b-5f6e7a8b9c0e";
    let mut second = carol.create_group(other).unwrap();
    let (_, welcome) = carol.add(&mut second, &[&last]).unwrap();
    carol.merge_pending_commit(&mut second).unwrap();
    let mut bobs_second = bob.join(&welcome, None).unwrap();

    let sealed = alice.encrypt(&mut first, b"one").unwrap();
    assert_eq!(
        bob.process(&mut bobs_first, &sealed).unwrap().plaintext,
        b"one"
    );
    let sealed = carol.encrypt(&mut second, b"two").unwrap();
    assert_eq!(
        bob.process(&mut bobs_second, &sealed).unwrap().plaintext,
        b"two"
    );
}
