#!/usr/bin/env python3

from livereload import Server, shell

server = Server()

# Watch the directory for changes, you can specify the path to your project directory
server.watch('.', delay=1)

# Optionally, you can specify a command to build your project, if needed
# server.watch('your_source.c', shell('emcc your_source.c -o output.html -s WASM=1', cwd='path/to/your/project'))

# Serve the current directory at the specified port
server.serve(root='.', port=8000)
