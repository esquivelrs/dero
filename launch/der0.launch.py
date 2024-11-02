from ament_index_python.packages import get_package_share_directory
from launch import LaunchDescription
from launch.substitutions import PathJoinSubstitution
from launch_ros.actions import Node

CONFIG_COMMON_DIR = PathJoinSubstitution([get_package_share_directory("dero"), "config"])


def generate_launch_description():
    params = [PathJoinSubstitution([CONFIG_COMMON_DIR, "dero_carried_1.yaml"])]

    
    std_points = Node(
        package="agrirobot_std_radar",
        executable="agrirobot_bosch_std_radar_node",
        name="agrirobot_bosch_std_radar_node",
        remappings=[
            ("/in", "/tractor/radar/bosch/bottom/points/sync"),
            ("/out", "/tractor/radar/front/points"),
        ],
    )  

    dero_std_points = Node(
        package="agrirobot_std_radar",
        executable="agrirobot_dero_std_radar_node",
        name="agrirobot_bosch_std_radar_node",
        remappings=[
            ("/in", "/sensor_platform/radar/scan"),
            ("/out", "/tractor/radar/front/points"),
        ],
    )
    
    dero = Node(
        package="dero",
        executable="scekf_dero_node",
        name="scekf_dero_node",
        namespace="radar",
        parameters=[
            params,
            {"use_sim_time": True},
        ],
        remappings=[
            ("/input_imu", "/sensor_platform/imu"), # /sensor_platform/imu /tractor/lidar/front/imu/sync
            ("/input_radar", "/tractor/radar/front/points"),
        ],
        # prefix=['xterm -e gdb -ex run --args']
    )


    return LaunchDescription(
        [
            std_points,
            dero_std_points,
            dero,
        ]
    )
