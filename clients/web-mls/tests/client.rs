// The client's core, natively: what the WebAssembly build runs, minus the bindings. The
// cross-implementation check, the browser build against the FFI bridge through a chat server,
// is interop/run.sh.

use web_mls::core::{Client, Error, Kind, inspect, members};

const ROOM: &[u8] = b"0192f0c4-8a1e-7c3a-9d2b-5f6e7a8b9c0d";

#[test]
fn two_devices_form_a_group_and_talk() {
    let alice = Client::new(b"alice").unwrap();
    let bob = Client::new(b"bob").unwrap();
    let mut group = alice.create_group(ROOM).unwrap();

    let package = bob.key_package().unwrap();
    let info = inspect(&package).unwrap();
    assert_eq!(info.wire_format, "key_package");
    assert_eq!(info.identity.as_deref(), Some(&b"bob"[..]));

    let (commit, welcome) = alice.add(&mut group, &[&package]).unwrap();
    // OpenMLS's default policy, as the bridge's: handshake messages are encrypted too.
    assert_eq!(inspect(&commit).unwrap().wire_format, "private_message");
    assert_eq!(inspect(&commit).unwrap().content_type, Some("commit"));
    assert_eq!(inspect(&commit).unwrap().epoch, Some(0));
    assert_eq!(inspect(&welcome).unwrap().wire_format, "welcome");
    alice.merge_pending_commit(&mut group).unwrap();

    // A welcome for someone else is refused, and changes nothing.
    let carol = Client::new(b"carol").unwrap();
    assert_eq!(carol.join(&welcome).err(), Some(Error::Rejected));

    let mut bobs = bob.join(&welcome).unwrap();
    assert_eq!(members(&bobs), vec![b"alice".to_vec(), b"bob".to_vec()]);
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
    let mut bobs = bob.join(&welcome).unwrap();
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
fn a_third_member_is_added_by_a_commit_the_others_process() {
    let alice = Client::new(b"alice").unwrap();
    let bob = Client::new(b"bob").unwrap();
    let carol = Client::new(b"carol").unwrap();
    let mut a = alice.create_group(ROOM).unwrap();
    let (_, welcome) = alice.add(&mut a, &[&bob.key_package().unwrap()]).unwrap();
    alice.merge_pending_commit(&mut a).unwrap();
    let mut b = bob.join(&welcome).unwrap();

    let (commit, welcome) = bob.add(&mut b, &[&carol.key_package().unwrap()]).unwrap();
    bob.merge_pending_commit(&mut b).unwrap();
    let seen = alice.process(&mut a, &commit).unwrap();
    assert_eq!(seen.kind, Kind::Commit);
    assert_eq!(seen.sender, b"bob");
    let mut c = carol.join(&welcome).unwrap();
    assert_eq!(a.epoch(), c.epoch());

    let sealed = carol.encrypt(&mut c, b"hi both").unwrap();
    assert_eq!(
        alice.process(&mut a, &sealed).unwrap().plaintext,
        b"hi both"
    );
    assert_eq!(bob.process(&mut b, &sealed).unwrap().plaintext, b"hi both");

    let commit = alice.remove(&mut a, b"carol").unwrap();
    alice.merge_pending_commit(&mut a).unwrap();
    bob.process(&mut b, &commit).unwrap();
    assert_eq!(carol.process(&mut c, &commit).unwrap().kind, Kind::Commit);
    let sealed = alice.encrypt(&mut a, b"just us").unwrap();
    assert_eq!(bob.process(&mut b, &sealed).unwrap().plaintext, b"just us");
    assert!(carol.process(&mut c, &sealed).is_err());
    assert_eq!(
        alice.remove(&mut a, b"carol").err(),
        Some(Error::NotAMember)
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
    assert_eq!(alice.join(&sealed).err(), Some(Error::Malformed));
}
