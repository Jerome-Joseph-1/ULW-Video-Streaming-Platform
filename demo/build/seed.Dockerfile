# The demo's rooms and member lists (demo/db/seed.sql), applied with psql once the schema is
# there, for demo/release/compose.yaml. Context: the repository root.
# postgres:16 (16.15), the digest demo/compose.yaml pins (it shares its layers with the database).
FROM docker.io/library/postgres@sha256:1a6ab3f5345eb6dbe04a1349529caabdb0ab09293a09590fad07b2246bfa4b54
COPY demo/db/seed.sql /seed.sql
CMD ["psql", "-v", "ON_ERROR_STOP=1", "-q", "-f", "/seed.sql", "postgresql://postgres:testtest123@127.0.0.1:5432/postgres"]
