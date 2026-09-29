# Built from this directory: docker build -t ulw/mock-auth deploy/local/mock-auth

# golang:1.25-bookworm, 2026-09. The service uses the standard library only, so the build
# fetches nothing; GOTOOLCHAIN=local keeps go.mod from asking for a toolchain download.
FROM golang:1.25-bookworm@sha256:3b4a11519ad929d1e1d261a12cff056f0c85b735253d7d861346b9c6f8b36437 AS build
WORKDIR /src
COPY go.mod main.go ./
RUN CGO_ENABLED=0 GOTOOLCHAIN=local go build -trimpath -o /out/mock-auth .

# distroless/static-debian12:nonroot, 2026-09.
FROM gcr.io/distroless/static-debian12:nonroot@sha256:afa5c872c891853ca7fcf1f12c3edb23f7eeef36189728842dd51042ff57f7ab
COPY --from=build /out/mock-auth /mock-auth
USER 65532:65532
ENTRYPOINT ["/mock-auth"]
