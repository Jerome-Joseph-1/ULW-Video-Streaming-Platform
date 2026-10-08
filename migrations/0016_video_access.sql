-- Who besides its owner may see a video (ADR-0097): a visibility per video (private, the members
-- of one chat room, or anyone signed in who has the id) and grants to single users, made by the
-- operator's backend. Until now only the uploader could see or play a video.
--
-- Applied after 0015 (chat_members' roles and its index by user), which this reads through the
-- room check: the migrator refuses a version older than the newest applied, so this file must
-- never reach a database before 0011 to 0015.
--
-- Order matters, as in 0015: the migrator runs this file in one transaction and holds every lock
-- a statement takes until the commit. The new table and its indexes come first. They are built
-- on a table with no rows, and the foreign key takes videos' SHARE ROW EXCLUSIVE lock, under
-- which every read of videos proceeds; only a video's own writes (an upload's progress, a
-- commit, the worker's transitions) wait, for the moments until the commit. The ALTER on videos
-- comes last, as one statement: two columns, one with a constant default and one without, and
-- two checks added NOT VALID, all catalog changes that scan and rewrite nothing, so its ACCESS
-- EXCLUSIVE lock, which does stop reads, is held only from there to the commit. No index is
-- built on videos: every lookup the access check makes goes by a primary key (videos,
-- chat_members, video_grants), and building one here would hold that lock for the whole build.

-- One row per user the operator's backend granted a video to. User ids compare bytewise ("C"),
-- as chat_members' do, so that pages of grants follow the order the service compares them in.
-- 1 to 128 of exactly the characters the service parses a user id from (core::UserId,
-- is_subject_char: A-Z a-z 0-9 . _ : @ | + -); an operator's INSERT is held to the same. The
-- table is new, so the check is validated at once.
CREATE TABLE video_grants (
    video_id   uuid NOT NULL REFERENCES videos (id) ON DELETE CASCADE,
    user_id    text COLLATE "C" NOT NULL,
    granted_at timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (video_id, user_id),
    CONSTRAINT video_grants_user_id CHECK (user_id ~ '^[A-Za-z0-9._:@|+-]{1,128}$')
);

-- A user's grants without reading the whole table: to revoke everything a user holds when the
-- product deletes them (RUNBOOK). The primary key leads with the video.
CREATE INDEX video_grants_by_user ON video_grants (user_id, video_id);

-- Every existing video, and every video inserted without naming them (an upload's create, a
-- live recording's, ADR-0092), is private: the default. The checks are NOT VALID so that
-- nothing scans videos: every existing row holds the default, and every new or updated row is
-- checked. A later migration validates both (RUNBOOK).
ALTER TABLE videos
    ADD COLUMN visibility text NOT NULL DEFAULT 'private',
    ADD COLUMN visibility_room uuid,
    ADD CONSTRAINT videos_visibility
        CHECK (visibility IN ('private', 'room', 'unlisted')) NOT VALID,
    ADD CONSTRAINT videos_visibility_room
        CHECK ((visibility = 'room') = (visibility_room IS NOT NULL)) NOT VALID;
