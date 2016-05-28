To build using docker, first install latest docker. Then do the following:

# symlink the desired dockerfile
ln -s Dockerfile.gcc49 Dockerfile

# build the container
sudo docker build -t jlib-build:gcc49 .

# run the container
sudo docker run -tiv `pwd`:/src/jlib jlib-build:gcc49

# from inside the container
cd /src/jlib

# configure
mkdir build-gcc49
cd build-gcc49
../configure

# build
make -j6
