pkill -f node2_server
lsof -i :12345
UCX tensfer has to send the buffer contents rather than buffer pointers 
Server node buffer has to be alive until the node shuts down