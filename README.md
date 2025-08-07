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
