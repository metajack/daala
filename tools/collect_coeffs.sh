#!/bin/bash

set -e

IMAGES=($*)

mkdir -p coeffs
parallel -j4 'examples/encoder_example {1} -o /dev/null > coeffs/{1/.}.coeffs 2> /dev/null ; grep ^4 coeffs/{1/.}.coeffs > coeffs/{1/.}.coeffs4 ; grep ^8 coeffs/{1/.}.coeffs > coeffs/{1/.}.coeffs8 ; grep ^16 coeffs/{1/.}.coeffs > coeffs/{1/.}.coeffs16 ; grep ^32 coeffs/{1/.}.coeffs > coeffs/{1/.}.coeffs32' ::: ${IMAGES[@]}
