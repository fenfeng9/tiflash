# Copyright 2025 PingCAP, Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

arch=$(uname -m)
export PATH="/opt/cmake/bin:/usr/local/bin/:${PATH}"
export LIBRARY_PATH="/usr/local/lib/${arch}-unknown-linux-gnu/:${LIBRARY_PATH:+LIBRARY_PATH:}"
export LD_LIBRARY_PATH="/usr/local/lib/${arch}-unknown-linux-gnu/:${LD_LIBRARY_PATH:+LD_LIBRARY_PATH:}"
export CPLUS_INCLUDE_PATH="/usr/local/include/${arch}-unknown-linux-gnu/c++/v1/:${CPLUS_INCLUDE_PATH:+CPLUS_INCLUDE_PATH:}"

SCRIPTPATH=$(cd $(dirname "$0"); pwd -P)
