# 0096. Member lists are changed under the room's lock, by their users where the operator allows it and by the operator's backend; presence is seen only within shared chats

Status: Accepted
Date: 2026-10-03, amended 2026-10-04 before merging (who may start a chat, the service API,
presence)
Amends: ADR-0054 (no client command changed a member list), ADR-0073 (the database tells nodes
of removals only), ADR-0056 (anyone signed in could watch anyone)

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

An integrating product (an operator that embeds ULW) then found two holes, and three more
questions followed from them:

- **Anyone could reach anyone.** With the commands above, any signed-in user opens a chat with,
  makes a group of, and so calls, anyone whose user id they know. A product that has its own
  request-and-accept step (a friend request, an invitation, a match) cannot keep it: its users
  bypass it by sending ULW the id. The ids are not secret; they are the identity provider's
  subjects, and they show in every `sender` and `member` frame.
- **Who lists people then.** If users may not, the product's backend must, and so needs a way in
  that is neither a user's socket nor SQL against ULW's database.
- **How the backend is known.** ULW keeps no credentials of its own (ADR-0018): users are whoever
  the identity provider's token says.
- **Presence was open.** ADR-0056 left "anyone signed in may watch anyone" as an open item:
  anyone could learn when anyone else is online by knowing their id.
- **Where presence would be checked, and what keeps it current.** Watches are answered by the
  watcher's node from what it hears in the watched user's presence room (ADR-0056); member lists
  live in the database, and change on any node or none.

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

**Who may start a chat** (added 2026-10-04)

| Option | Why it was tempting | Verdict |
|---|---|---|
| Users always may, as above | Simplest; what the demo does | Rejected as the only mode: an embedding product cannot keep its own request and accept |
| A request and accept of ULW's own (an `invite`, an `accept`) | Self-contained | Rejected: every product's rule differs (friends, matches, a company directory, a paid tier), and a second, different step beside the product's own is worse than none |
| An operator setting, off by default, plus an API for the operator's backend that lists people | The product keeps its rule and lists the pair once it allows; a demo or closed community turns users on | Accepted ("backend API and switch") |

**The backend's API**

| Option | Why it was tempting | Verdict |
|---|---|---|
| SQL against `chat_members`, as the RUNBOOK's repairs | Exists already; the triggers notify every node | Rejected: a product would need ULW's database credentials and the store's statements (caps, kinds, roles, the room's lock and the creator's advisory lock) by heart; any mistake is a list no command could have made |
| HTTP on the gateway | The public HTTP surface already | Rejected: the member-list logic and the message store's connection are chat's; the gateway would need both, and its public routes and their cookie and CSRF rules are no place for a server-to-server caller |
| HTTP on chat's client port under a path | No new port | Rejected, as ADR-0093 rejected it for LiveKit's webhooks: chat's client port speaks one upgrade per connection with no body, one widened route makes it public, and its per-user limits do not fit a backend |
| HTTP/1.1 on a listener of chat's own (`ULW_SERVICE_PORT`), off unless configured, POST with a JSON body per operation, keep-alive, one request at a time per connection | Any chat node takes any request; the operations call the store exactly as the socket's commands do; the transport mirrors ADR-0093's webhook listener | Accepted |
| A static shared secret for the backend | Easy for an operator | Rejected: ULW keeps no credentials (ADR-0018); a secret in two places is one more to rotate and to leak |
| The identity provider's token with a claim the operator names (`ULW_SERVICE_CLAIM`, default `scope`, holding `ULW_SERVICE_SCOPE`; OAuth's space-separated `scope` matched by word: the settings and `infra/auth/service_claim.hpp` every ULW service that takes service calls shares), from the client-credentials grant | The same verifier, keys, rotation and SIGHUP as users' tokens; every provider issues such tokens | Accepted |
| The backend acting through an admin's identity (`add_members` "as" a user) | No new authority | Rejected: the product's rule, not a group admin's, decides; and a direct chat has no admin |

**Who may see whose presence**

| Option | Why it was tempting | Verdict |
|---|---|---|
| Anyone may watch anyone (ADR-0056) | Free | Rejected: an integrating product's users could watch strangers' online status by id |
| A contact list kept by ULW | Explicit | Rejected: ULW keeps no directory (ADR-0018); the product already has one, and the shared chats are what it listed |
| Users who share a direct or group chat now | Follows the lists the product (or the users) made, with nothing new to store | Accepted ("shared rooms only") |
| Check at the watched user's node, filtering announcements | One place per user | Rejected: the presence room carries no client identities (ADR-0056), and a node would have to know every watcher's lists |
| Check at the watcher's node, before a watch is let in; on any removal, hold back every watch here that involves the user removed and ask again | Each node polices its own clients; removals already reach every node (above); nothing is cached that could go stale | Accepted |
| Cache each watch's basis (which shared room) and drop only those whose room lost a member | Fewer reads | Rejected: the reads are one indexed query per client affected, and a cache is one more thing to keep right across resyncs |

## Decision

- **Commands** (docs/integration/chat.md, "Member lists"): `open_direct` (`user`), answered
  `direct`; `create_group` (`id`, optional `users`), answered `group`; `add_members` (`room`,
  `users`), answered `added` with those it listed; `remove_member` (`room`, `user`), answered
  `removed`; `leave` (`room`), answered `left`; `rooms` (optional `after`, `limit`), answered
  `rooms`; `members` (`room`, optional `after`, `limit`), answered `members`. None needs a join
  first. A refusal is an `error` with `not_member`, `not_admin`, `not_group`,
  `too_many_members`, `room_limit`, `gone`, `self`, `rate_limited` (with `retry_after_ms`) or
  `unavailable`.
- **Named rooms.** A direct chat's room is the first 16 bytes of SHA-256 over
  `ulw direct chat\n`, the two ids in byte order and a newline between them (none after), with
  the first byte replaced by the tag `0x03` and the RFC 9562 version 8 and variant bits set
  (byte 6's high nibble `8`, byte 8's high bits `10`); a group chat's likewise over
  `ulw group chat\n`, the creator, a newline and the request's id, tagged `0x04`
  (`apps/chat/src/named_rooms.cpp`, `core::ports::NamedRoom`). The tag replaces the digest's
  first byte rather than preceding it, unlike a stream's chat (ADR-0070). Such a room is only ever its
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
- **Self-service is the operator's** (`ULW_CHAT_SELF_SERVICE`, `on` or `off`, default `off`;
  `ServiceLimits::self_service`). Off, `open_direct`, `create_group` and `add_members` from a
  socket are refused at once with `not_allowed`, before the allowance or the store
  (`membership_refusals_total{reason="not_allowed"}`). `leave`, `remove_member` (still an
  admin's alone) and the listings are unchanged either way. Off is the default because it is the
  safe one for an embedder; the demo and the test stacks that use the commands set it on.
- **The service API** (`apps/chat/src/service_api.cpp`, docs/integration/chat.md, "The service
  API"). `ULW_SERVICE_PORT`, which needs `ULW_SERVICE_SCOPE` and `ULW_SERVICE_CLIENT_ID` set
  (chat exits 2 otherwise): a scope alone admits whichever client the provider grants it to.
  `POST /service/v1/` `open_direct` (`users`: the pair), `create_group` (`creator`, `id`,
  `users`), `add_members` (`room`, `users`), `remove_member` (`room`, `user`), `close_direct`
  (`room`), `rooms` (`user`, `after`, `limit`), `members` (`room`, `after`, `limit`), answered in
  JSON with the socket's shapes. Each calls the store as the socket's commands do, under the
  room's lock, with the same caps and triggers:
  - `open_direct` and `create_group` with the first user and the creator as the asker (so the
    per-user room cap counts them);
  - `add_members` and `roster` with `Actor::service()`, which the store treats as every group's
    admin and as listed (`kAddMembers`' and `kRoster`'s service flag). `add_members` takes only a
    group's own id (`04`; anything else is `bad_room` before the store), and only a group that
    lists someone: one nothing recorded, or a refused join recorded and nobody created, is
    `no_room`, and one whose members all left over its history is `gone`, as `create_group` is;
  - `remove_member` as the user's `leave` (`kLeave`), so a group keeps an admin; idempotent,
    answering `removed: []` for someone not listed;
  - `close_direct` (`kCloseDirect`, a `03` id only): both of a direct chat's pair unlisted under
    its lock, so each removal is told to every node, their sockets leave the room and their
    watches of each other are checked again. This is how a product unfriends or blocks: a direct
    chat's pair otherwise never changes, and **a user may not leave a direct chat themselves**
    (`not_group`, as before): a one-sided leave would leave the other in a room with nobody, and
    whether the two may still talk is the product's to decide. A later `open_direct` lists the
    pair again, in the same room with its history. With self-service on, either may do that, so
    self-service on is also "anyone can reach and watch anyone whose id they know".
  Rooms are the named rooms above: the same pair or request names the same room whoever asks.
  Authentication: `Authorization: Bearer` only (no cookie), the users' verifier, then
  `Claims::is_service`, which `infra/auth` sets (`claim_holds`, `names_client`) when the claim
  `ULW_SERVICE_CLAIM` names (default `scope`) is a string equal to `ULW_SERVICE_SCOPE`, a
  space-separated string holding it as a word, or an array holding it, and `azp` or `client_id`
  names `ULW_SERVICE_CLIENT_ID`; never with no scope. `401` (with `WWW-Authenticate: Bearer`),
  `403` for a valid token that is not the service's, `503` when the keys cannot be fetched.
  Bounded per node, in the order a request meets them: 32 connections, at most 8 from one
  address, closed at accept past either; a first byte within 5 s; a whole request within 10 s,
  16 KiB at most; a failures' budget per caller address (20 at once, 5 a second), charged before
  the token is verified and given back once it is the service's, past which the address is
  answered `429` without a verification; then the service's own rate, 100 at once and 50 a
  second, taken only by verified service requests, so no stranger can spend it. 30 s idle; one
  request at a time per connection, the next read once the answer is out. Metrics
  `service_api_*`. The port has no HTTPRoute: nothing on the internet needs it, and the token
  alone admits a caller; an operator whose backend is outside the cluster routes it privately,
  or, through the public Gateway, only with a source restriction (RUNBOOK, step 10).
- **Presence within shared chats** (`apps/chat/src/presence.cpp`, `IPresenceAccess`,
  `IMessageStore::shared_with`, `kSharedWith`, read through `chat_members_by_user` and the
  primary key, which a test EXPLAINs). A `watch` is let in only once the store says the two
  share a direct or group chat now (a room with no kind recorded counts as a group; a stream's
  live chat never does); until then the node neither answers nor joins the user's presence room,
  and a refusal is `not_shared` (`unavailable` if the store cannot say). The node is the
  watcher's, which alone knows its clients; every node enforces it for its own, so a cluster
  needs no coordination beyond what removals already have.
  - **Cost and what it reveals.** Every watch the store is asked about costs the watcher's
    allowance (128 at once, then 2 a second), whether or not anyone on the node watches the
    target already, and the node's room cap is applied after the store's answer: neither a
    refusal nor its timing says whether the target is watched there. A `not_shared` is
    remembered per (watcher, target), up to 8192 pairs, and answered from memory until either is
    listed in any chat (`on_member_added`, which `ChatService` now passes on too) or the node
    resyncs, so asking again costs no query.
  - **Removals.** On a removal from any list (the notification every node hears; `ChatService`
    passes it on, as it does to the call handler), every watch on this node that involves the
    removed user, as watcher or as watched, is held back at once (no `presence` reaches it), and
    its client's watches are asked about again in one query; a watch no longer shared is dropped
    and its client told `not_shared` unasked, and one still shared resumes with a `presence` for
    whatever changed while held. A removal that comes while a check is out marks it stale, asked
    again when answered, so no answer read before a removal lets a watch in or back.
  - **Outages fail closed without dropping.** A held watch the store cannot check stays held,
    telling nothing, and is asked again after 1 s, 2 s, 4 s, ... up to every 30 s, until the
    store answers. A resync (lost notifications) holds every watch at once and asks about them
    32 connections at a time, a wave each 100 ms, so a node coming back does not send its whole
    load to a database that has just come back.
  Pending checks count against the 128 watches per connection. Metrics `presence_checks_total`,
  `presence_refusals_total{reason="not_shared"}`, `presence_watches_revoked_total`.
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

- Where the operator turns self-service on, clients open conversations, make groups and manage
  them without an operator; where it is off (the default), the product's backend lists people
  through the service API, and users still leave, remove as admins and list. The RUNBOOK's SQL
  remains for repairs and for granting admin.
- A deployment that used the commands before this amendment (a demo) must set
  `ULW_CHAT_SELF_SERVICE=on`, or its clients get `not_allowed`.
- Anyone holding a token with the service claim can list anyone in any chat: the claim must be
  granted to the backend's client alone (RUNBOOK, step 10).
- A `watch` costs one indexed read, and is answered a round trip later than before. A removal
  costs, on each node, one read per connection whose watches involve the user removed; a resync
  one per connection that watches anyone.
- Adding someone to a chat does not start a watch; clients watch again after a `member`
  `added`.
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
