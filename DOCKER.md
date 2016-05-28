To build using docker, first install latest docker. Then do the following:

# symlink the desired dockerfile
ln -s Dockerfile.gcc49 Dockerfile

# build the container
sudo docker build -t jlib-build:4.9 .

# run the container
sudo docker run -tiv /home/dragon/src/jlib:/src/jlib jlib-build:4.9

# from inside the container
cd /src/jlib

# configure
mkdir build-gcc49
cd build-gcc49
../configure

# build
make -j6
