# SWARM-ARENA

Forked from `swarm-playground` from ZJU-FAST lab, to test path planning algorithms

## Setup
**Step 1** Setup workspace
```bash
mkdir -p ~/swarm_ws/src
cd ~/swarm_ws/src
git clone git@github.com:mamariomiamo/swarm_arena.git
git submodule update --init --recursive
cd ~/swarm_ws
catkin build quadrotor_msgs # at <your_ws>
catkin build
```
**For Batch testing** 
In a terminal
```bash
cd ~/swarm_ws/src/swarm-arena
source d2_run.sh
```

**For demo testing**
In the first terminal, launch rviz visualization module
```bash
cd ~/swarm_ws
source devel/setup.bash
roslaunch ego_planner rviz.launch
```

In the second terminal, launch map generation module
```bash
cd ~/swarm_ws
source devel/setup.bash
roslaunch map_generator pcd2cloud.launch pcd_file:=d2_obstacle_map.pcd
```

In the third terminal, launch planning module
```bash
cd ~/swarm_ws
source devel/setup.bash
roslaunch ego_planner rviz.launch
```

## References 
1. Swarm-playground https://github.com/ZJU-FAST-Lab/EGO-Planner-v2
2. Spherical SFC generation https://github.com/mamariomiamo/corridor_gen
3. Map generation https://github.com/mamariomiamo/map_gen
