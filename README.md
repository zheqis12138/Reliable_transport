# Reliable_transport

The project implement a reliable transport protocol at the transport layer, above UDP protocol, to add more reliablility to the UDP. Following 
features are added: 
• Handle packet drops
• Handle packet corruption
• Provide trivial flow control
• Provide a stream abstraction
• Allow multiple packets to be outstanding at any time (using a limit given to your program as a run-time parameter, via the -w option)
• Handle packet reordering
