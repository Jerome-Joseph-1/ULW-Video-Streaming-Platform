package main

import (
	"crypto"
	"crypto/rsa"
	"crypto/sha256"
	"math/big"
	"strings"
	"testing"
	"time"
)

func rsaPublicKey(t *testing.T, i *issuer) *rsa.PublicKey {
	t.Helper()
	for _, jwk := range i.jwks()["keys"] {
		if jwk["kty"] != "RSA" {
			continue
		}
		if jwk["alg"] != "PS256" {
			t.Fatalf("the RSA key publishes alg %q, want PS256", jwk["alg"])
		}
		n, err := b64.DecodeString(jwk["n"])
		if err != nil {
			t.Fatal(err)
		}
		e, err := b64.DecodeString(jwk["e"])
		if err != nil {
			t.Fatal(err)
		}
		return &rsa.PublicKey{N: new(big.Int).SetBytes(n), E: int(new(big.Int).SetBytes(e).Int64())}
	}
	t.Fatal("no RSA key published")
	return nil
}

func TestRSATokensAreSignedWithPSS(t *testing.T) {
	i := &issuer{name: "issuer", audience: "audience"}
	if _, err := i.rotate(); err != nil {
		t.Fatal(err)
	}
	token, err := i.mint("PS256", "subject", "", time.Minute)
	if err != nil {
		t.Fatal(err)
	}
	cut := strings.LastIndexByte(token, '.')
	signature, err := b64.DecodeString(token[cut+1:])
	if err != nil {
		t.Fatal(err)
	}
	digest := sha256.Sum256([]byte(token[:cut]))
	public := rsaPublicKey(t, i)
	options := rsa.PSSOptions{SaltLength: rsa.PSSSaltLengthEqualsHash, Hash: crypto.SHA256}
	if err := rsa.VerifyPSS(public, crypto.SHA256, digest[:], signature, &options); err != nil {
		t.Fatalf("not a PSS signature with a hash-length salt: %v", err)
	}
	if rsa.VerifyPKCS1v15(public, crypto.SHA256, digest[:], signature) == nil {
		t.Fatal("verifies as PKCS #1 v1.5")
	}
}

func TestRS256IsNotIssued(t *testing.T) {
	i := &issuer{name: "issuer", audience: "audience"}
	if _, err := i.rotate(); err != nil {
		t.Fatal(err)
	}
	if _, err := i.mint("RS256", "subject", "", time.Minute); err == nil {
		t.Fatal("minted an RS256 token")
	}
}
