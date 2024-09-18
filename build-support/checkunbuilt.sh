#!/bin/bash

pkgs=($(ls ./pkgs/))
recipes=($(ls ./recipes/))
unbuilt=($(comm -3 <(printf "%s\n" "${recipes[@]}" | sort) <(printf "%s\n" "${pkgs[@]}" | sort) | sort -n))

echo "${unbuilt[@]}"
