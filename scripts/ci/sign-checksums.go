package main

import (
	"crypto/ed25519"
	"encoding/hex"
	"flag"
	"fmt"
	"os"
	"strings"
)

func main() {
	keyHex := flag.String("key", "", "Ed25519 private key hex string")
	checksumsFile := flag.String("checksums", "dist/SHA256SUMS.txt", "Path to checksums file")
	sigFile := flag.String("out", "dist/SHA256SUMS.txt.sig", "Path to output signature file")
	expectedPubHex := flag.String("pubkey", "538eefb498971db790422d53d24aa1ed2623e37298ef6c9dfd436b739cf5aa3c", "Expected Ed25519 public key hex")
	flag.Parse()

	if strings.TrimSpace(*keyHex) == "" {
		fmt.Fprintln(os.Stderr, "::error::CRITICAL SECURITY FAILURE: ENGINE_RELEASE_PRIVATE_KEY secret is not provided!")
		os.Exit(1)
	}

	checksumsData, err := os.ReadFile(*checksumsFile)
	if err != nil {
		fmt.Fprintf(os.Stderr, "::error::Failed to read checksums file %s: %v\n", *checksumsFile, err)
		os.Exit(1)
	}

	privKeyBytes, err := hex.DecodeString(strings.TrimSpace(*keyHex))
	if err != nil {
		fmt.Fprintf(os.Stderr, "::error::Failed to decode private key hex: %v\n", err)
		os.Exit(1)
	}

	var privKey ed25519.PrivateKey
	if len(privKeyBytes) == ed25519.PrivateKeySize {
		privKey = ed25519.PrivateKey(privKeyBytes)
	} else if len(privKeyBytes) == ed25519.SeedSize {
		privKey = ed25519.NewKeyFromSeed(privKeyBytes)
	} else {
		fmt.Fprintf(os.Stderr, "::error::Invalid private key length: expected 32 or 64 bytes, got %d\n", len(privKeyBytes))
		os.Exit(1)
	}

	// Compute signature
	sig := ed25519.Sign(privKey, checksumsData)

	// Verify immediately against expected public key
	pubKeyBytes, err := hex.DecodeString(strings.TrimSpace(*expectedPubHex))
	if err != nil || len(pubKeyBytes) != ed25519.PublicKeySize {
		fmt.Fprintf(os.Stderr, "::error::Invalid expected public key: %v\n", err)
		os.Exit(1)
	}

	if !ed25519.Verify(pubKeyBytes, checksumsData, sig) {
		fmt.Fprintln(os.Stderr, "::error::CRITICAL SECURITY FAILURE: Generated signature does NOT match expected public key!")
		os.Exit(1)
	}

	if err := os.WriteFile(*sigFile, sig, 0644); err != nil {
		fmt.Fprintf(os.Stderr, "::error::Failed to write signature file %s: %v\n", *sigFile, err)
		os.Exit(1)
	}

	fmt.Printf("✓ Cryptographic Ed25519 signature generated and verified against %s\n", *expectedPubHex)
	fmt.Printf("✓ Signature saved to %s (%d bytes)\n", *sigFile, len(sig))
}
