# NATS-Connector

Practical appliance of nats.c library as a connector service in another project.

Here's the **documentation** about deploying a NATS Server: https://docs.nats.io/running-a-nats-service/introduction  
Here's **link** with latest NATS server release: https://github.com/nats-io/nats-server/releases

note: if you using Windows and don't want to rummage in documentation - choose "**...-windows-amd64.zip**" as a standalone version  
just unpack it and run in terminal: <code>```./nats-server ```</code>

### For building:
For now as a prerequisite you should install **nats.c package** into your system.  
In my case i did it through vcpkg (in windows terminal): <code>```vcpkg install cnats```</code>  
However later in plans is to remove requirement for nats.c package and install it automatically through CMakeLists.  

Next - build it using **CMake** (it's already configured). Or whatever tool you want.

Binary file you can find in "**build**" folder.  
For now project doesn't have any options(parameters), so you can run it in terminal just by it's name: <code>```./nats-connector```</code>  

### For testing:
Make sure to enable testing option in CMake file first:  
<code>set(ENABLE_TESTS OFF CACHE BOOL "Build unit tests" FORCE)</code> OFF -> ON.  
Now you can build project again - CMake will automatically download and build necessary library (gtest) for you.  
Executable for tests should be found in the same "**build**" folder or you can access to them in VS Code Testing tab.  