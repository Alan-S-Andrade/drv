# Build the Docker Image

`docker build -t <tag> . -f docker/Dockerfile`

# Start the Docker Container
`docker run -dti <tag>`

# Connect to the Docker Container
1. Find the container id with `docker ps`
2. Connect with `docker attach <container-id>`
