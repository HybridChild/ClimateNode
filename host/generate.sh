#!/usr/bin/env bash
# Generate node_pb2.py from the shared contract in proto/.
#
# Run after any change to proto/node.proto. The output is generated code and is
# gitignored -- never hand-edit it, and never let it drift from the .proto.
#
# Uses grpc_tools.protoc rather than a system protoc so the generated code and
# the installed protobuf runtime always agree on version. See requirements.txt.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
proto_dir="${here}/../proto"

if [[ ! -d "${here}/.venv" ]]; then
	echo "error: ${here}/.venv not found -- create it first:" >&2
	echo "  python3 -m venv host/.venv" >&2
	echo "  host/.venv/bin/pip install -r host/requirements.txt" >&2
	exit 1
fi

"${here}/.venv/bin/python" -m grpc_tools.protoc \
	--proto_path="${proto_dir}" \
	--python_out="${here}" \
	node.proto

echo "generated ${here}/node_pb2.py from ${proto_dir}/node.proto"
