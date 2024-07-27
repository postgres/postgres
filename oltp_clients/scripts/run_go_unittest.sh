set -e

script_dir=$(dirname "$0")
FC_DIR="$script_dir/../"
echo "Running go unit tests under $FC_DIR"


function test_storage()
{
  echo " Go unittest for storage start."
  go test -race -cover -v "${FC_DIR}/storage/..." -failfast -count=1
  echo " Go unittest for storage finished."
}

function test_acp()
{
  echo " Go unittest for commit protocol start."
  echo " Go unittest for coordinator-side of transaction branches."
  go test -race -v -cover "${FC_DIR}/network/participant/..." -failfast -count=1
  echo " Go unittest for participant-side of transaction branches."
  go test -race -v -cover "${FC_DIR}/network/coordinator/..." -failfast -count=1
  echo " Go unittest for commit protocol finished."
}

test_storage
test_acp
echo " Go unittest finished"
