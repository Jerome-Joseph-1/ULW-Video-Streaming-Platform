# 0096. Member lists are changed by their users, over the room WebSocket, under the room's lock

Status: Accepted
Date: 2026-10-03
Amends: ADR-0054 (no client command changed a member list), ADR-0073 (the database tells nodes
of removals only)

## Context

Direct and group chats admit only their listed members (ADR-0054), and until now only an
operator could list one, in SQL (RUNBOOK section 3): a client had no way to open a conversation
with someone, make a group, or leave one. The 1:1 call test listed its pair by SQL too. Nothing in
the brief or in the chat milestones (M16 to M19) specifies the commands, so this ADR designs
them in the shape of the existing envelope. Six things had to be settled:

- **Which room a direct chat is.** Two people opening a chat with each other at once, on two
  nodes, must land in one room, and the same pair must always find the same room.
- **Who a target user is.** Identities come from the identity provider's tokens (ADR-0018):
  there is no table of users to look a target up in.
- **Who may change a list.** The schema had members and nothing else; no creator, no roles.
- **Where a change is decided.** The call handler reads membership on the room's owner, at the
  moment of asking (`IMessageStore::access`, ADR-0087); a removal reaches the nodes by
  notification (ADR-0073). Concurrent changes must not pass a check only one of them would.
- **Who hears of a change.** The members, to update what they show, and for end-to-end encrypted
  rooms to change the MLS group (ADR-0016, ADR-0038).
- **What a client may do with it.** A row per member per room, and a notification to every node
  per change, are what a script would try to multiply.

## Options

| Option | Why it was tempting | Verdict |
|---|---|---|
| Commands on the room WebSocket, beside `join` and `call` | Clients already hold the socket, authenticated, rate limited and answered in one envelope; chat has no HTTP API but its health endpoints | Accepted |
| A small HTTP API on chat (`POST /rooms`, ...) | Familiar REST shapes | Rejected: a second authenticated surface on chat (cookie, origin and CSRF rules, ADR-0078), for commands a client sends from the socket it already has |
| A direct chat's room minted at random, found again through a pair table with a unique index | Room ids stay version 7 (ADR-0023) | Rejected: a table and a race to resolve; the pair already determines the room |
| A direct chat's room derived from the pair, as a stream's chat is from its name (ADR-0070) | Every node, and a client, computes the same room with nothing looked up; opening it twice anywhere names one room | Accepted |
| A group's room minted at random | Simple | Rejected: a create repeated after a lost answer would make a second group; deriving it from the creator and the request's id makes the repeat idempotent, as a message id does a send |
| Check that the target user exists | Fewer rooms with nobody behind them | Rejected: nothing knows the users (ADR-0018), and a user who has never signed in is indistinguishable from one who does not exist; the product, which has the directory, resolves names to ids |
| Route each change through the room's owner over the node channel (`ask_owner`, ADR-0087) | One serial point per room | Rejected: a room nobody has joined has no owner yet; the database serialises the change by a row lock just as well, and is where every node already hears changes from (ADR-0073) |
| Read the list, then write in a second statement, without a lock | One round trip fewer | Rejected: two adds both pass the size cap, and an admin removed a moment ago can still add |
| Lock the room's `chat_rooms` row, then decide and write in one statement, in a transaction | Every change reads the list as the previous change committed it; refusals write nothing | Accepted |
| Logic in a PL/pgSQL function per change | One round trip | Rejected: the statements live where the store's other statements do (`message_sql.hpp`), and change with the code rather than with a migration |
| Notify additions through the room's owner, to the room's subscribers | Members of a room that is joined hear it in the room | Rejected: a user added to a room they have not joined is subscribed nowhere; a trigger on `chat_members`, as for removals, reaches every node whoever changed the row |

## Decision

- **Commands** (docs/integration/chat.md, "Member lists"): `open_direct` (`user`), answered
  `direct`; `create_group` (`id`, optional `users`), answered `group`; `add_members` (`room`,
  `users`), answered `added` with those it listed; `remove_member` (`room`, `user`), answered
  `removed`; `leave` (`room`), answered `left`; `rooms` (optional `after`, `limit`), answered
  `rooms`; `members` (`room`, optional `after`, `limit`), answered `members`. None needs a join
  first. A refusal is an `error` with `not_member`, `not_admin`, `not_group`,
  `too_many_members`, `room_limit`, `gone`, `self`, `rate_limited` (with `retry_after_ms`) or
  `unavailable`.
- **Named rooms.** A direct chat's room is a version 8 UUID tagged `0x03`, the rest SHA-256 over
  `ulw direct chat\n`, the two ids in byte order and a newline between them; a group chat's is
  tagged `0x04`, over `ulw group chat\n`, the creator, a newline and the request's id
  (`apps/chat/src/named_rooms.cpp`, `core::ports::NamedRoom`). Such a room is only ever its
  kind: a join of it asks for that kind whatever it says (`"kind"` naming the other is
  `bad_room`), the room plane creates an unrecorded one as it (`kind_of_unrecorded`), and the
  database refuses anything else (`chat_rooms_named_kind`, migration 0015). Nobody can take a
  pair's room by recording its id first as a group chat.
- **A direct chat** lists its pair when it lists nobody yet (just recorded, or recorded by a
  refused join before anyone opened it); otherwise it changes nothing and answers whether the
  asker is listed, so a member an operator removed is not put back by the other opening it
  again. Its pair never changes: `add_members`, `remove_member` and `leave` answer `not_group`.
  Opening a chat with oneself is `self`.
- **Target users are not validated** beyond being user ids. A room with someone who never signs
  in is rows nobody reads; the per-user room cap below bounds how many a user makes.
- **A group's id is not reused over its history.** A `create_group` that finds its room listing
  nobody but holding messages (everyone left) lists nobody and answers `gone`: listing new
  members would hand them the old history. The client creates the group under a new id.
- **Roles.** `chat_members.role` is `member` or `admin` (migration 0015, default `member`, so
  every row from before and every operator insert without one is a member). A group's creator is
  its admin. Only an admin adds or removes others (`not_admin`); anyone listed may leave; an
  admin removing themselves is leaving. When the last admin leaves and anyone is left, the
  member whose id sorts first becomes admin in the same statement, so a group with members
  always has one. The `left` answer names the member promoted, and the role change is told to
  everyone concerned as a `member` frame with `change` `promoted` (`demoted` for an operator
  taking admin away). Granting admin to more members is left to operators (`UPDATE chat_members SET
  role = 'admin'`) until a client needs it.
- **Caps.** A group holds at most 100 members (`kMaxGroupMembers`, a small group in ADR-0016's
  sense); an add that would pass it adds nobody (`too_many_members`). One create or add names at
  most 50 users (`malformed` past it). Listings page 1 to 100 entries, 50 by default, in byte
  order of room id or user id, with `more` saying whether another page follows: the service
  asks the store for one entry more than the page, and the stores answer up to
  `kMaxListPage + 1`, so a full page of 100 still knows.
- **A durable cap per user.** Whoever asks `open_direct` or `create_group` must be listed in
  fewer than 1000 rooms (`kMaxRoomsPerUser`) for it to list anyone, or it answers
  `room_limit`. It is counted in the database, through `chat_members_by_user`, inside the
  change's transaction and under an advisory lock on the user (`kLockCreator`, taken before the
  room's lock and only by these two changes, so no lock order inverts), so it holds across every
  node and over any length of time. Being added by others is not bounded by it, so nobody can
  shut someone else out of new rooms.
- **Under the room's lock.** Each change is a transaction: `BEGIN`; for `open_direct` and
  `create_group`, the room recorded as its kind if nothing recorded it (`kEnsureRoom`); the
  room's `chat_rooms` row locked `FOR UPDATE` (`kLockRoom`), after the creator's advisory lock
  for the two changes that may create a room; one statement that checks the
  asker's role, the room's kind and the cap against the list as it is now (READ COMMITTED gives
  each statement a fresh snapshot) and writes only if they pass; `COMMIT`. The answer goes out
  after the commit. A change only ever locks one room, so changes cannot deadlock one another.
  The call handler's `access` read on the room's owner (ADR-0087) reads the same rows at each
  ask, so a ticket is never issued on a membership a change has already taken away; nothing on
  any node caches a list.
- **Notifications.** Migration 0015's trigger sends `+ <room> <user>` or `- <room> <user>` on
  the `chat_members` channel for every row inserted, deleted or moved, however it changed, and
  `* <room> <role> <user>` for a row whose role alone changed. `PgMessageStore` listens there instead of on
  `chat_member_removed` (whose 0009 trigger stays for nodes from before, until none runs).
  Each node tells, with an unasked `member` frame (`room`, `user`, `change`), every socket of the
  user named and every socket in the room; a removal still takes the user's sockets out of the
  room first (ADR-0073). A stream's live chat has no list to tell of.
- **End-to-end encryption.** The server's list is who may read and send; an MLS group's roster
  is the clients' (ADR-0016), and no server code reads, checks or makes a commit. After an add,
  the device that added (or, when it cannot, any member's device that hears the `member` event)
  fetches a KeyPackage of each new member's devices (ADR-0038) and sends the Commit and the
  Welcome as ordinary messages in the room; the new member joins, reads the Welcome from
  history and applies commits in seq order. After a removal or a leave, a remaining member's
  device commits the Remove; the first valid Commit for an epoch in seq order wins (e2ee.md). A
  member who leaves cannot remove themselves from the MLS group, which is why the others are
  told. What the server enforces: only listed members join, send or read history (ADR-0054);
  a removed member is cut off at once on every node (ADR-0073), so ciphertext sent after the
  removal never reaches them even before the Remove commit; a direct chat is always the same
  two; a group stays small enough for every commit to reach every member. Clients compare the
  MLS roster with `members` and treat a difference as a commit owed.
- **Allowances.** Changes cost the user's membership allowance: 20 at once, then one each 3 s
  (`ServiceLimits::membership_burst`, `membership_interval`), past which a change is
  `rate_limited` with `retry_after_ms`. Listings are reads of the store and cost the join
  allowance, as history pages do. Like every chat allowance these are kept per node, in memory:
  a user with sockets on N nodes gets N times them, and a node's restart refills them. They pace
  load; what bounds the rows a user can make is the durable cap above.
- **Metrics:** `directs_opened_total`, `groups_created_total`,
  `members_changed_total{change="added"|"removed"|"left"}`,
  `membership_refusals_total{reason=...}` and `member_events_total`.
- **Migration 0015**, after 0011 to 0014 (live streams, then group calls; the migrator refuses
  a version older than the newest applied, so it never runs before them). The migrator runs a migration
  in one transaction and holds every lock to its commit, so the order of its statements is the
  order of its locks: first the index `chat_members (user_id, room_id)`, built under CREATE
  INDEX's SHARE lock, during which member writes wait and reads (joins, history, listings) go
  on; then the catalog-only changes, the `role` column with its default and the checks on
  `chat_members` (role; user id at most 128 bytes with no white space) and `chat_rooms` (named
  kind), all `NOT VALID` so nothing is scanned; then the trigger. Their ACCESS EXCLUSIVE locks,
  which do stop reads, are taken last and held only for the moments until the commit. A unit
  test keeps the index ahead of every ALTER. A later migration validates the checks.

## Consequences

- Clients open conversations, make groups and manage them without an operator; the RUNBOOK's
  SQL remains for repairs and for granting admin.
- A change is four to six round trips to Postgres holding one row lock, a few milliseconds; at
  20 at once and one each 3 s per user, the lock is never the bottleneck of a room.
- Every node hears every change and looks through its clients for those concerned, as it does
  for removals: a create of 51 users is 51 notifications, each a walk of at most 1,280 clients.
  The allowance bounds it at about 17 notifications a second per user at the burst's end.
- A rolling deploy that brings 0015 has old nodes hear removals on the old channel and new
  nodes on the new one; a member added by a new node's command reaches an old node's sockets as
  nothing (the old node never told of additions), which is what happened before.
- Building `chat_members_by_user` holds a SHARE lock on `chat_members` for the length of the
  build: member changes wait, joins and other reads do not. Deploy 0015 off-peak (RUNBOOK).
- A user id named by mistake gets a room the other person never sees; the product shows rooms
  only for ids it resolved.
- Reopen if a group needs more than one admin managed by clients, if group size must grow past
  what one MLS commit fan-out comfortably carries, or if a user directory appears that targets
  should be checked against.
