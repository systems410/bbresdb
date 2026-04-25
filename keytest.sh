
#!/usr/bin/env bash 
bazel-bin/service/tools/kv/api_tools/kv_service_tools --config service/tools/config/interface/service.config --cmd set --key key1 --value value1
bazel-bin/service/tools/kv/api_tools/kv_service_tools --config service/tools/config/interface/service.config --cmd get --key key1