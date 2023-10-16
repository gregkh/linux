#!/bin/busybox sh

echo "tcrypt: starting CRYPTO_SHA2_HACL"
modprobe tcrypt mode=300 alg=sha224-hacl sec=2
modprobe tcrypt mode=300 alg=sha256-hacl sec=2
modprobe tcrypt mode=300 alg=sha384-hacl sec=2
modprobe tcrypt mode=300 alg=sha512-hacl sec=2

echo "tcrypt: starting SHA2 (256) test"
echo "tcrypt: testing sha256 generic implementation"
modprobe tcrypt mode=300 alg=sha256-generic sec=2

echo "tcrypt starting SHA2 (512) test"
echo "tcrypt: testing sha512 generic implementation"
modprobe tcrypt mode=300 alg=sha512-generic sec=2

echo "tcrypt: starting SHA3 (256) test"
echo "tcrypt: testing sha3-256 generic implementation"
modprobe tcrypt mode=300 alg=sha3-256-generic sec=2

echo "tcrypt: starting SHA3 (512) test"
echo "tcrypt: testing sha3-512 generic implementation"
modprobe tcrypt mode=300 alg=sha3-512-generic sec=2

echo "tcrypt: starting BLAKE2b (256) test"
echo "tcrypt: testing blake2b-256 generic implementation"
modprobe tcrypt mode=300 alg=blake2b-256-generic sec=2

echo "tcrypt: starting BLAKE2b (512) test"
echo "tcrypt: testing blake2b-512 generic implementation"
modprobe tcrypt mode=300 alg=blake2b-512-generic sec=2