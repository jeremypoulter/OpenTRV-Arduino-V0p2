#!/bin/sh -e
# Build the config specified by PIO_ENV

if [ -z $PIO_ENV ]; then
  echo "PIO_ENV not set"
  exit 1
fi

platformio run -e $PIO_ENV
