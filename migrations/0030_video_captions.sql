-- A video's captions (ADR-0139): one WebVTT file per language, uploaded by its owner or the
-- operator's backend and stored next to its renditions, at videos/<id>/hls/captions/<language>.vtt.
-- A row says the file is there and that the master playlist lists it; the gateway writes the
-- file before the row and removes it after the row.
--
-- One new table and nothing else: it is built empty, and its foreign key takes videos' SHARE ROW
-- EXCLUSIVE lock, under which every read of videos proceeds, as 0016's video_grants did. A video's
-- rows go with it at the reaper's purge (ON DELETE CASCADE); its file goes with the rest of
-- videos/<id>/.
--
-- The language is a lowercase BCP 47 tag of the plain shape core::CaptionLanguage parses: 2 or 3
-- letters, then subtags of 1 to 8 letters or digits, each after a '-', at most 35 characters. It
-- compares bytewise ("C"), the order the master playlist lists the languages in.
CREATE TABLE video_captions (
    video_id   uuid NOT NULL REFERENCES videos (id) ON DELETE CASCADE,
    language   text COLLATE "C" NOT NULL,
    updated_at timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (video_id, language),
    CONSTRAINT video_captions_language
        CHECK (length(language) <= 35 AND language ~ '^[a-z]{2,3}(-[a-z0-9]{1,8})*$')
);
