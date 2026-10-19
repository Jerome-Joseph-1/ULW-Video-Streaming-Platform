# 0123. The service sets a member's role

Status: Accepted
Date: 2026-10-19
Amends: ADR-0096 (a group's roles change by the service as well as by a leave)

## Context

A group's creator is its admin. After that, roles changed only when the last admin left and the
member whose id sorts first took over (ADR-0096). An operator who wanted another admin, or
wanted to take the role away from someone, had to edit `chat_members` by hand. That matters more
now that a group's admins moderate its calls (ADR-0115) (#218).

## Options

| Option | For it | Against it | Verdict |
|---|---|---|---|
| A service API operation, `set_role`, under the room's lock as every other change | The backend's own policy decides who is admin; the same `member` frames as today | One more operation to keep in step with the store | Accepted |
| Admins promote and demote others from the socket | Self-service | Who may make whom an admin is product policy; it can come later on top of the store change | Deferred |

## Decision

- `POST /service/v1/set_role` takes `{room, user, role}`. `role` is `admin` or `member`, and
  `room` must be a group's id. It answers `{"type":"role",...,"changed":...}`.
- The change runs under the room's lock, like the other member-list changes. The trigger that
  already announces role changes tells every node, and every socket concerned hears `member`
  `promoted` or `demoted`. A repeat changes nothing and tells nobody.
- A group keeps an admin while it has members. Demoting its only admin is `409` `last_admin`,
  and changes nothing.

## Consequences

- An operator changes roles without SQL, and its members see the change at once.
- Handing a group to someone else takes two calls: promote them, then demote the old admin.
