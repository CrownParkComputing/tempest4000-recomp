#!/usr/bin/env bash
# Run the pure-Java importer regression without Android or a game dump.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
classes=$(mktemp -d)
trap 'rm -rf "$classes"' EXIT
javac -d "$classes" android/java/com/crownparkcomputing/ps4runtime/DriverZipImporter.java tests/DriverZipImporterTest.java
java -cp "$classes" com.crownparkcomputing.ps4runtime.DriverZipImporterTest "$@"
