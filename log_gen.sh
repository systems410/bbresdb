#!/usr/bin/env bash 



output_file="shard_log.txt"                                                                                                                                                                                       
echo "" > "$output_file" 

service_logs="${1:-.}"
                                                                                                                                                                                                                  
for dir in "$service_logs"/*; do                                                                                                                                                                                               
    [[ ! -d "$dir" ]] && continue                                                                                                                                                                                
    log=$(grep -E "(2PC|PBFT)" "$dir/kv_service.log")                                                                                                                                                            
    node=$(basename "$dir")
    echo " ---- Node $node ---- " >> "$output_file"                                                                                                                                                               
    echo "$log" >> "$output_file"                                                                                                                                                                                
    echo "" >> "$output_file"                                                                                                                                                                                    
done                                                                                                                                                                                                             