// mock-auth stands in for Askedin's auth-service inside the sandbox cluster. It signs tokens
// the way the real service does (the claims our verifier reads, the stage cookie) and publishes
// its keys at /.well-known/jwks.json, which the gateway fetches over https exactly as it would
// fetch Askedin's. It signs with every algorithm the gateway accepts, each with a key of its own
// whose JWK names that algorithm: an RSA key for RS256, another for PS256, an EC P-256 key for
// ES256 and an Ed25519 key for EdDSA, so each is exercised through the cluster. It rotates all
// four on demand.
//
//	GET  /.well-known/jwks.json   current keys and the generation before them
//	POST /token?sub=&email=&alg=&ttl=
//	                              {"token":...} plus the cookie; alg is RS256, PS256, ES256 or EdDSA
//	POST /rotate                  new keys; tokens signed before stay valid for one rotation
//	GET  /healthz
//	GET  /whoami                  the x-user-* headers it was sent, as JSON; the sandbox routes
//	                              it behind the header-stripping policies that mimic
//	                              askedin-gateway (deploy/local/cluster/askedin-identity.yaml)
//
// Test infrastructure only: the keys live in memory and die with the pod.
package main

import (
	"crypto"
	"crypto/ecdsa"
	"crypto/ed25519"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/rsa"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"errors"
	"flag"
	"log"
	"math/big"
	"net/http"
	"strconv"
	"sync"
	"time"
)

// A week, as ulw_devtoken allows: long enough for a sandbox left up over a weekend.
const maxTTL = 7 * 24 * time.Hour

var b64 = base64.RawURLEncoding

type signingKey struct {
	kid  string
	alg  string
	jwk  map[string]string
	sign func(digest []byte, signingInput []byte) ([]byte, error)
}

// The kid is the RFC 7638 thumbprint of the public key, as ulw_devtoken derives it, so the
// same key always carries the same kid.
func thumbprint(members string) string {
	sum := sha256.Sum256([]byte(members))
	return b64.EncodeToString(sum[:])
}

// An RSA key signs for one algorithm only, and its JWK says which, so the gateway sees a token
// whose alg does not fit the key's refused rather than verified with the wrong padding.
func newRSA(alg string, sign func(*rsa.PrivateKey, []byte) ([]byte, error)) (signingKey, error) {
	key, err := rsa.GenerateKey(rand.Reader, 2048)
	if err != nil {
		return signingKey{}, err
	}
	n := b64.EncodeToString(key.N.Bytes())
	e := b64.EncodeToString(big.NewInt(int64(key.E)).Bytes())
	kid := thumbprint(`{"e":"` + e + `","kty":"RSA","n":"` + n + `"}`)
	return signingKey{
		kid:  kid,
		alg:  alg,
		jwk:  map[string]string{"kty": "RSA", "n": n, "e": e},
		sign: func(digest, _ []byte) ([]byte, error) { return sign(key, digest) },
	}, nil
}

// RS256 pads with PKCS #1 v1.5. The gateway accepts it, as it accepts PS256, so the sandbox
// signs with both.
func newRS256() (signingKey, error) {
	return newRSA("RS256", func(key *rsa.PrivateKey, digest []byte) ([]byte, error) {
		return rsa.SignPKCS1v15(rand.Reader, key, crypto.SHA256, digest)
	})
}

func newPS256() (signingKey, error) {
	return newRSA("PS256", func(key *rsa.PrivateKey, digest []byte) ([]byte, error) {
		// RFC 7518 section 3.5: the salt is as long as the hash.
		options := rsa.PSSOptions{SaltLength: rsa.PSSSaltLengthEqualsHash}
		return rsa.SignPSS(rand.Reader, key, crypto.SHA256, digest, &options)
	})
}

func newEC() (signingKey, error) {
	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		return signingKey{}, err
	}
	// P-256 coordinates and signature halves are 32 bytes, left-padded (RFC 7518 3.4, 6.2.1).
	const size = 32
	pad := func(v *big.Int) []byte { return v.FillBytes(make([]byte, size)) }
	x := b64.EncodeToString(pad(key.X))
	y := b64.EncodeToString(pad(key.Y))
	kid := thumbprint(`{"crv":"P-256","kty":"EC","x":"` + x + `","y":"` + y + `"}`)
	return signingKey{
		kid: kid,
		alg: "ES256",
		jwk: map[string]string{"kty": "EC", "crv": "P-256", "x": x, "y": y},
		sign: func(digest, _ []byte) ([]byte, error) {
			r, s, err := ecdsa.Sign(rand.Reader, key, digest)
			if err != nil {
				return nil, err
			}
			return append(pad(r), pad(s)...), nil
		},
	}, nil
}

func newEd25519() (signingKey, error) {
	public, private, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		return signingKey{}, err
	}
	x := b64.EncodeToString(public)
	kid := thumbprint(`{"crv":"Ed25519","kty":"OKP","x":"` + x + `"}`)
	return signingKey{
		kid: kid,
		alg: "EdDSA",
		jwk: map[string]string{"kty": "OKP", "crv": "Ed25519", "x": x},
		// Ed25519 signs the message itself, not a digest of it.
		sign: func(_, signingInput []byte) ([]byte, error) {
			return ed25519.Sign(private, signingInput), nil
		},
	}, nil
}

func newGeneration() (map[string]signingKey, error) {
	generation := map[string]signingKey{}
	for _, generate := range []func() (signingKey, error){newRS256, newPS256, newEC, newEd25519} {
		key, err := generate()
		if err != nil {
			return nil, err
		}
		generation[key.alg] = key
	}
	return generation, nil
}

type issuer struct {
	name     string
	audience string
	cookie   string

	mu       sync.Mutex
	current  map[string]signingKey
	previous map[string]signingKey
}

func (i *issuer) rotate() ([]string, error) {
	next, err := newGeneration()
	if err != nil {
		return nil, err
	}
	i.mu.Lock()
	defer i.mu.Unlock()
	i.previous, i.current = i.current, next
	kids := make([]string, 0, len(next))
	for _, key := range next {
		kids = append(kids, key.kid)
	}
	return kids, nil
}

func (i *issuer) jwks() map[string][]map[string]string {
	i.mu.Lock()
	defer i.mu.Unlock()
	keys := []map[string]string{}
	for _, generation := range []map[string]signingKey{i.current, i.previous} {
		for _, key := range generation {
			jwk := map[string]string{"kid": key.kid, "alg": key.alg, "use": "sig"}
			for name, value := range key.jwk {
				jwk[name] = value
			}
			keys = append(keys, jwk)
		}
	}
	return map[string][]map[string]string{"keys": keys}
}

func (i *issuer) mint(alg, subject, email string, ttl time.Duration) (string, error) {
	i.mu.Lock()
	key, ok := i.current[alg]
	i.mu.Unlock()
	if !ok {
		return "", errors.New("alg must be RS256, PS256, ES256 or EdDSA")
	}
	header, err := json.Marshal(map[string]string{"alg": key.alg, "typ": "JWT", "kid": key.kid})
	if err != nil {
		return "", err
	}
	now := time.Now().Unix()
	claims := map[string]any{
		"iss": i.name,
		"aud": i.audience,
		"sub": subject,
		"iat": now,
		"nbf": now,
		"exp": now + int64(ttl/time.Second),
	}
	if email != "" {
		claims["email"] = email
	}
	payload, err := json.Marshal(claims)
	if err != nil {
		return "", err
	}
	input := b64.EncodeToString(header) + "." + b64.EncodeToString(payload)
	digest := sha256.Sum256([]byte(input))
	signature, err := key.sign(digest[:], []byte(input))
	if err != nil {
		return "", err
	}
	return input + "." + b64.EncodeToString(signature), nil
}

func writeJSON(w http.ResponseWriter, status int, body any) {
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "no-store")
	w.WriteHeader(status)
	if err := json.NewEncoder(w).Encode(body); err != nil {
		log.Printf("response: %v", err)
	}
}

func fail(w http.ResponseWriter, status int, reason string) {
	writeJSON(w, status, map[string]string{"error": reason})
}

func (i *issuer) routes() *http.ServeMux {
	mux := http.NewServeMux()
	mux.HandleFunc("GET /healthz", func(w http.ResponseWriter, _ *http.Request) {
		w.WriteHeader(http.StatusOK)
	})
	mux.HandleFunc("GET /whoami", func(w http.ResponseWriter, r *http.Request) {
		writeJSON(w, http.StatusOK, map[string][]string{
			"x-user-id":    r.Header.Values("X-User-Id"),
			"x-user-email": r.Header.Values("X-User-Email"),
		})
	})
	mux.HandleFunc("GET /.well-known/jwks.json", func(w http.ResponseWriter, _ *http.Request) {
		writeJSON(w, http.StatusOK, i.jwks())
	})
	mux.HandleFunc("POST /token", func(w http.ResponseWriter, r *http.Request) {
		q := r.URL.Query()
		subject := q.Get("sub")
		if subject == "" {
			fail(w, http.StatusBadRequest, "sub is required")
			return
		}
		alg := q.Get("alg")
		if alg == "" {
			alg = "ES256"
		}
		ttl := time.Hour
		if text := q.Get("ttl"); text != "" {
			seconds, err := strconv.ParseInt(text, 10, 64)
			if err != nil || seconds < 1 || time.Duration(seconds)*time.Second > maxTTL {
				fail(w, http.StatusBadRequest, "ttl must be whole seconds up to a week")
				return
			}
			ttl = time.Duration(seconds) * time.Second
		}
		token, err := i.mint(alg, subject, q.Get("email"), ttl)
		if err != nil {
			fail(w, http.StatusBadRequest, err.Error())
			return
		}
		http.SetCookie(w, &http.Cookie{
			Name:     i.cookie,
			Value:    token,
			Path:     "/",
			MaxAge:   int(ttl / time.Second),
			HttpOnly: true,
			Secure:   true,
			SameSite: http.SameSiteLaxMode,
		})
		writeJSON(w, http.StatusOK, map[string]string{"token": token})
	})
	mux.HandleFunc("POST /rotate", func(w http.ResponseWriter, _ *http.Request) {
		kids, err := i.rotate()
		if err != nil {
			fail(w, http.StatusInternalServerError, err.Error())
			return
		}
		writeJSON(w, http.StatusOK, map[string][]string{"kids": kids})
	})
	return mux
}

func main() {
	name := flag.String("issuer", "", "iss of every token, and what JWT_ISSUER must say")
	audience := flag.String("audience", "askedin-platform", "aud of every token")
	cookie := flag.String("cookie", "auth_token_stage", "cookie /token sets")
	plain := flag.String("listen", ":8080", "plain HTTP listener, reached through the Gateway")
	secure := flag.String("listen-tls", ":8443", "HTTPS listener the verifiers fetch keys from")
	cert := flag.String("tls-cert", "", "certificate chain for -listen-tls")
	key := flag.String("tls-key", "", "private key for -listen-tls")
	flag.Parse()
	if *name == "" || *cert == "" || *key == "" {
		log.Fatal("-issuer, -tls-cert and -tls-key are required")
	}

	i := &issuer{name: *name, audience: *audience, cookie: *cookie}
	if _, err := i.rotate(); err != nil {
		log.Fatalf("generate keys: %v", err)
	}
	mux := i.routes()
	// Timeouts so a stuck client cannot hold a connection forever; nothing here is slow.
	const timeout = 10 * time.Second
	servers := []*http.Server{
		{Addr: *plain, Handler: mux, ReadHeaderTimeout: timeout, WriteTimeout: timeout},
		{Addr: *secure, Handler: mux, ReadHeaderTimeout: timeout, WriteTimeout: timeout},
	}
	errs := make(chan error, len(servers))
	go func() { errs <- servers[0].ListenAndServe() }()
	go func() { errs <- servers[1].ListenAndServeTLS(*cert, *key) }()
	log.Printf("mock-auth issuer=%s audience=%s cookie=%s", *name, *audience, *cookie)
	log.Fatal(<-errs)
}
