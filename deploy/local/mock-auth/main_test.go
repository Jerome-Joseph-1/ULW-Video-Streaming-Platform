package main

import (
	"crypto"
	"crypto/ecdsa"
	"crypto/ed25519"
	"crypto/elliptic"
	"crypto/rsa"
	"crypto/sha256"
	"encoding/json"
	"math/big"
	"strings"
	"testing"
	"time"
)

// Every algorithm the gateway accepts (docs/integration/auth.md, "What a token must be").
var accepted = []string{"RS256", "PS256", "ES256", "EdDSA"}

func newIssuer(t *testing.T) *issuer {
	t.Helper()
	i := &issuer{name: "issuer", audience: "audience"}
	if _, err := i.rotate(); err != nil {
		t.Fatal(err)
	}
	return i
}

func decode(t *testing.T, text string) []byte {
	t.Helper()
	raw, err := b64.DecodeString(text)
	if err != nil {
		t.Fatal(err)
	}
	return raw
}

func number(t *testing.T, text string) *big.Int {
	t.Helper()
	return new(big.Int).SetBytes(decode(t, text))
}

// verify checks a token against the published key set the way the gateway does: the header's
// kid picks the key, the key's own alg must be the header's, and the signature must verify
// with that algorithm. It returns the kid.
func verify(t *testing.T, i *issuer, token string) string {
	t.Helper()
	parts := strings.Split(token, ".")
	if len(parts) != 3 {
		t.Fatalf("not a compact JWS: %q", token)
	}
	var header map[string]string
	if err := json.Unmarshal(decode(t, parts[0]), &header); err != nil {
		t.Fatal(err)
	}
	var jwk map[string]string
	for _, candidate := range i.jwks()["keys"] {
		if candidate["kid"] == header["kid"] {
			jwk = candidate
		}
	}
	if jwk == nil {
		t.Fatalf("kid %q is not in the key set", header["kid"])
	}
	alg := header["alg"]
	if jwk["alg"] != alg {
		t.Fatalf("a %s token signed with a key that publishes alg %q", alg, jwk["alg"])
	}
	input := []byte(parts[0] + "." + parts[1])
	signature := decode(t, parts[2])
	digest := sha256.Sum256(input)
	switch alg {
	case "RS256", "PS256":
		if jwk["kty"] != "RSA" {
			t.Fatalf("%s: key %v", alg, jwk)
		}
		public := &rsa.PublicKey{N: number(t, jwk["n"]), E: int(number(t, jwk["e"]).Int64())}
		pkcs1 := rsa.VerifyPKCS1v15(public, crypto.SHA256, digest[:], signature)
		// RFC 7518 section 3.5: MGF1 with SHA-256 and a salt as long as the hash.
		options := rsa.PSSOptions{SaltLength: rsa.PSSSaltLengthEqualsHash, Hash: crypto.SHA256}
		pss := rsa.VerifyPSS(public, crypto.SHA256, digest[:], signature, &options)
		if alg == "RS256" && (pkcs1 != nil || pss == nil) {
			t.Fatalf("RS256 is not PKCS #1 v1.5 alone: pkcs1 %v, pss %v", pkcs1, pss)
		}
		if alg == "PS256" && (pss != nil || pkcs1 == nil) {
			t.Fatalf("PS256 is not PSS alone: pss %v, pkcs1 %v", pss, pkcs1)
		}
	case "ES256":
		if jwk["kty"] != "EC" || jwk["crv"] != "P-256" || len(signature) != 64 {
			t.Fatalf("ES256: key %v, signature of %d bytes", jwk, len(signature))
		}
		public := &ecdsa.PublicKey{
			Curve: elliptic.P256(), X: number(t, jwk["x"]), Y: number(t, jwk["y"]),
		}
		r := new(big.Int).SetBytes(signature[:32])
		s := new(big.Int).SetBytes(signature[32:])
		if !ecdsa.Verify(public, digest[:], r, s) {
			t.Fatal("ES256 does not verify")
		}
	case "EdDSA":
		if jwk["kty"] != "OKP" || jwk["crv"] != "Ed25519" {
			t.Fatalf("EdDSA: key %v", jwk)
		}
		if !ed25519.Verify(ed25519.PublicKey(decode(t, jwk["x"])), input, signature) {
			t.Fatal("EdDSA does not verify")
		}
	default:
		t.Fatalf("unexpected alg %q", alg)
	}
	return header["kid"]
}

func TestEveryAcceptedAlgorithmVerifiesAgainstTheKeySet(t *testing.T) {
	i := newIssuer(t)
	kids := map[string]string{}
	for _, alg := range accepted {
		token, err := i.mint(alg, "subject", "", time.Minute)
		if err != nil {
			t.Fatalf("%s: %v", alg, err)
		}
		kid := verify(t, i, token)
		if other, seen := kids[kid]; seen {
			t.Fatalf("%s and %s share kid %q", alg, other, kid)
		}
		kids[kid] = alg
	}
}

func TestEachKeyPublishesItsOwnAlgorithm(t *testing.T) {
	i := newIssuer(t)
	published := map[string]int{}
	for _, jwk := range i.jwks()["keys"] {
		published[jwk["alg"]]++
	}
	for _, alg := range accepted {
		if published[alg] != 1 {
			t.Fatalf("%d keys publish alg %s, want 1", published[alg], alg)
		}
	}
	if len(published) != len(accepted) {
		t.Fatalf("published algorithms %v, want exactly %v", published, accepted)
	}
}

func TestTokensFromThePreviousKeysStillVerify(t *testing.T) {
	i := newIssuer(t)
	tokens := map[string]string{}
	for _, alg := range accepted {
		token, err := i.mint(alg, "subject", "", time.Minute)
		if err != nil {
			t.Fatal(err)
		}
		tokens[alg] = token
	}
	if _, err := i.rotate(); err != nil {
		t.Fatal(err)
	}
	for _, alg := range accepted {
		verify(t, i, tokens[alg])
	}
}

func TestAnAlgorithmTheGatewayRefusesIsNotIssued(t *testing.T) {
	i := newIssuer(t)
	for _, alg := range []string{"none", "HS256", "RS384", "ES384"} {
		if _, err := i.mint(alg, "subject", "", time.Minute); err == nil {
			t.Fatalf("minted a %s token", alg)
		}
	}
}
