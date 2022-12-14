#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <math.h> /* round, floor, ceil, trunc */

#include "rclcpp/rclcpp.hpp"
#include "std_msgs/msg/float64_multi_array.hpp"
#include "sensor_msgs/msg/joint_state.hpp"
#include "custom_interfaces/srv/set_joint_velocity.hpp"
#include "custom_interfaces/srv/set_end_effector_velocity.hpp"

using namespace std::chrono_literals;
using std::placeholders::_1;

/* ----------------------- Robot Parameter Definition ----------------------- */
class velocity_controller : public rclcpp::Node
{
public:
  velocity_controller() : Node("velocity_controller_server"), count_(0)
  {
    joint_velocity_service_ = this->create_service<custom_interfaces::srv::SetJointVelocity>("joint_velocity_service", std::bind(&velocity_controller::set_joint_velocity_from_service, this, _1));
    end_effector_velocity_service_ = this->create_service<custom_interfaces::srv::SetEndEffectorVelocity>("end_effector_velocity_service", std::bind(&velocity_controller::set_end_effector_velocity_from_service, this, _1));
    joint_state_subscriber_ = create_subscription<sensor_msgs::msg::JointState>("/joint_states", 10, std::bind(&velocity_controller::calculate_joint_efforts, this, _1));
    velocity_publisher_ = create_publisher<std_msgs::msg::Float64MultiArray>("/forward_effort_controller/commands", 10);
    reference_joint_publisher_ = create_publisher<std_msgs::msg::Float64MultiArray>("/reference_velocities/joints", 10);
    reference_end_effector_publisher_ = create_publisher<std_msgs::msg::Float64MultiArray>("/reference_velocities/end_effector", 10);
    end_effector_velocities_publisher_ = create_publisher<std_msgs::msg::Float64MultiArray>("/end_effector_velocities", 10);
  }

private:
  void set_joint_velocity_from_service(const std::shared_ptr<custom_interfaces::srv::SetJointVelocity::Request> request)
  {
    request_joint_velocity = {request->vq1, request->vq2, request->vq3};
    RCLCPP_INFO(rclcpp::get_logger("rclcpp"), "Request (vq1,vq2,vq3): ('%f','%f','%f')", request_joint_velocity[0], request_joint_velocity[1], request_joint_velocity[2]);
    service_call_for_joint_velocity_control = true;
  }

  void set_end_effector_velocity_from_service(const std::shared_ptr<custom_interfaces::srv::SetEndEffectorVelocity::Request> request)
  {
    request_end_effector_velocity = {request->vx, request->vy, request->vz};
    RCLCPP_INFO(rclcpp::get_logger("rclcpp"), "Request (vx,vy,vz): ('%f','%f','%f')", request_end_effector_velocity[0], request_end_effector_velocity[1], request_end_effector_velocity[2]);
    service_call_for_joint_velocity_control = false;
  }

  std::double_t find_average_error(std::vector<std::double_t> error_filter) 
  {
    std::double_t sum = 0;
    for (int i = 0; i < FILTER_LENGTH; i++) {
      sum += error_filter[i];
    }
    std::double_t average = sum / std::double_t(FILTER_LENGTH);
    return average;
  }

  std::double_t cap_value(std::double_t max, std::double_t value) {
    std::double_t capped_value = std::max(-max, std::min(max, value));
    return capped_value;
  }

  void calculate_joint_efforts(const sensor_msgs::msg::JointState::SharedPtr msg)
  {
    end = msg->header.stamp.nanosec;
    RCLCPP_INFO(rclcpp::get_logger("rclcpp"), "\n\n\n\n");

    std::vector<std::double_t> joint_position = {
        msg->position[0],
        msg->position[1],
        msg->position[2]};

    std::vector<std::double_t> joint_velocity = {
        msg->velocity[0],
        msg->velocity[1],
        msg->velocity[2]};

    if (service_call_for_joint_velocity_control)
    {
      request_end_effector_velocity[0] = -request_joint_velocity[0]*(sin(joint_position[0] + joint_position[1]) + (9.0*sin(joint_position[0]))/10.0) - request_joint_velocity[1]*sin(joint_position[0] + joint_position[1]);
      request_end_effector_velocity[1] = request_joint_velocity[0]*(cos(joint_position[0] + joint_position[1]) + (9.0*cos(joint_position[0]))/10.0) + request_joint_velocity[1]*cos(joint_position[0] + joint_position[1]);
      request_end_effector_velocity[2] = request_joint_velocity[2];

      RCLCPP_INFO(rclcpp::get_logger("rclcpp"), "Requested End Effector Velocity (vx,vy,vz): ('%f','%f','%f')", request_end_effector_velocity[0], request_end_effector_velocity[1], request_end_effector_velocity[2]);
    }
    else
    {
      request_joint_velocity[0] = (10*request_end_effector_velocity[0]*cos(joint_position[0] + joint_position[1]) + 10*request_end_effector_velocity[1]*sin(joint_position[0] + 
                                    joint_position[1]))/(9*sin(joint_position[1]));
      request_joint_velocity[1] = - (request_end_effector_velocity[0]*(10.0*cos(joint_position[0] + joint_position[1]) + 9.0*cos(joint_position[0])))/(9.0*sin(joint_position[1])) - 
                                      (request_end_effector_velocity[1]*(10.0*sin(joint_position[0] + joint_position[1]) + 9.0*sin(joint_position[0])))/(9.0*sin(joint_position[1]));
      request_joint_velocity[2] = request_end_effector_velocity[2];

      RCLCPP_INFO(rclcpp::get_logger("rclcpp"), "Actual Joint Velocity (vq1,vq2,vq3): ('%f','%f','%f')", joint_velocity[0], joint_velocity[1], joint_velocity[2]);
      RCLCPP_INFO(rclcpp::get_logger("rclcpp"), "Requested Joint Velocity (vq1,vq2,vq3): ('%f','%f','%f')", request_joint_velocity[0], request_joint_velocity[1], request_joint_velocity[2]);
    }

    std::vector<std::double_t> error = {
      joint_velocity[0] - request_joint_velocity[0],
      joint_velocity[1] - request_joint_velocity[1],
      joint_velocity[2] - request_joint_velocity[2]};

    low_pass_error_filter0[current_oldest_index] = error[0];
    low_pass_error_filter1[current_oldest_index] = error[1];
    low_pass_error_filter2[current_oldest_index] = error[2];

    if (current_oldest_index < (FILTER_LENGTH - 1)) {
      current_oldest_index++;
    } else {
      current_oldest_index = 0;
    }
    std::vector<std::double_t> filtered_error = {
      find_average_error(low_pass_error_filter0),
      find_average_error(low_pass_error_filter1),
      find_average_error(low_pass_error_filter2)
    };

    RCLCPP_INFO_STREAM(rclcpp::get_logger("rclcpp"), "filtered_err0: " << filtered_error[0] << ", filtered_err1: " << filtered_error[1] << ", filtered_err2" << filtered_error[2]);


    std::double_t duration = (end - begin) / 1000000000.0f;
    // RCLCPP_INFO(rclcpp::get_logger("rclcpp"), "Duration: '%f'", duration);

    std::vector<std::double_t> error_dot = {
        (last_iteration_joint_error[0] - filtered_error[0]) / duration,
        (last_iteration_joint_error[1] - filtered_error[1]) / duration,
        (last_iteration_joint_error[2] - filtered_error[2]) / duration};

    // RCLCPP_INFO(rclcpp::get_logger("rclcpp"), "Calculating Efforts");
    if (abs(error[0]) > acceptable_error) // Joint 1
      apply_joint_efforts[0] = cap_value(max_force, -(proportional_gain[0] * filtered_error[0]) - (derivative_gain[0] * error_dot[0]));

    if (abs(error[1]) > acceptable_error) // Joint 2
      apply_joint_efforts[1] = cap_value(max_force, -(proportional_gain[1] * filtered_error[1]) - (derivative_gain[1] * error_dot[1]));

    if (msg->position[2] >= 2.1 && request_joint_velocity[2] >= 0)
      apply_joint_efforts[2] = 9.8;
    else if (msg->position[2] <= 0.1 && request_joint_velocity[2] <= 0)
      apply_joint_efforts[2] = 9.8;
    else if (abs(error[2]) > acceptable_error) // Joint 3
      apply_joint_efforts[2] = cap_value(max_force, -(proportional_gain[2] * filtered_error[2]) - (derivative_gain[2] * error_dot[2]));

    std_msgs::msg::Float64MultiArray reference_joint_velocities;
    reference_joint_velocities.data = request_joint_velocity;
    reference_joint_publisher_->publish(reference_joint_velocities);

    std_msgs::msg::Float64MultiArray reference_end_effector_velocities;
    reference_end_effector_velocities.data = request_end_effector_velocity;
    reference_end_effector_publisher_->publish(reference_end_effector_velocities);

    RCLCPP_INFO(rclcpp::get_logger("rclcpp"), "Errors (q1,q2,q3): ('%f','%f','%f')", filtered_error[0], filtered_error[1], filtered_error[2]);
    RCLCPP_INFO(rclcpp::get_logger("rclcpp"), "Publishing Joint Efforts (u1,u2,u3): ('%f','%f','%f')", apply_joint_efforts[0], apply_joint_efforts[1], apply_joint_efforts[2]);

    std_msgs::msg::Float64MultiArray message;
    message.data.clear();
    message.data = apply_joint_efforts;
    velocity_publisher_->publish(message);
    begin = msg->header.stamp.nanosec;
    last_iteration_joint_error = filtered_error;

    std::vector<std::double_t> end_effector_velocity = {0, 0, 0}; // x,y,z
    end_effector_velocity[0] = -joint_velocity[0]*(sin(joint_position[0] + joint_position[1]) + (9.0*sin(joint_position[0]))/10.0) - joint_velocity[1]*sin(joint_position[0] + joint_position[1]);
    end_effector_velocity[1] = joint_velocity[0]*(cos(joint_position[0] + joint_position[1]) + (9.0*cos(joint_position[0]))/10.0) + joint_velocity[1]*cos(joint_position[0] + joint_position[1]);
    end_effector_velocity[2] = request_joint_velocity[2];

    std_msgs::msg::Float64MultiArray end_effector_velocities;
    end_effector_velocities.data = end_effector_velocity;
    end_effector_velocities_publisher_->publish(end_effector_velocities);
    RCLCPP_INFO(rclcpp::get_logger("rclcpp"), "Actual End Effector Velocity (vx,vy,vz): ('%f','%f','%f')", end_effector_velocity[0], end_effector_velocity[1], end_effector_velocity[2]);
  }

  // Variable Definition for class
  rclcpp::Service<custom_interfaces::srv::SetJointVelocity>::SharedPtr joint_velocity_service_;
  rclcpp::Service<custom_interfaces::srv::SetEndEffectorVelocity>::SharedPtr end_effector_velocity_service_;
  rclcpp::Subscription<sensor_msgs::msg::JointState>::SharedPtr joint_state_subscriber_;
  rclcpp::Publisher<std_msgs::msg::Float64MultiArray>::SharedPtr velocity_publisher_;
  rclcpp::Publisher<std_msgs::msg::Float64MultiArray>::SharedPtr reference_joint_publisher_;
  rclcpp::Publisher<std_msgs::msg::Float64MultiArray>::SharedPtr reference_end_effector_publisher_;
  rclcpp::Publisher<std_msgs::msg::Float64MultiArray>::SharedPtr end_effector_velocities_publisher_;

  bool pub_sub_uninitialized_ = true;
  bool service_call_for_joint_velocity_control = true;
  size_t count_;
  std::vector<std::double_t> request_joint_velocity = {0, 0, 0};
  std::vector<std::double_t> request_end_effector_velocity = {0, 0, 0};
  std::vector<std::double_t> last_iteration_joint_error = {0, 0, 0};
  std::vector<std::double_t> apply_joint_efforts = {0, 0, 9.8};
  std::vector<std::double_t> low_pass_error_filter0 = {0};
  std::vector<std::double_t> low_pass_error_filter1 = {0};
  std::vector<std::double_t> low_pass_error_filter2 = {0};
  const int FILTER_LENGTH = 1;
  int current_oldest_index = 0;
  std::uint64_t begin, end;

  std::double_t acceptable_error = 0.02f;
  std::double_t max_force = 20.0;
  std::double_t max_derivative_force = 3.0;
  std::vector<std::double_t> proportional_gain = {50, 20, 10}; // 50
  std::vector<std::double_t> derivative_gain = {0.15, 0, 0};  // 0.15
};

int main(int argc, char *argv[])
{
  rclcpp::init(argc, argv);
  auto node = std::make_shared<velocity_controller>();
  RCLCPP_INFO(rclcpp::get_logger("rclcpp"), "Starting End Effector Velocity Control.");
  system("ros2 run rrbot_gazebo switch_eff");
  system("ros2 topic pub --once /forward_effort_controller/commands std_msgs/msg/Float64MultiArray 'data: [0,0,0]'");
  rclcpp::spin(node);
  rclcpp::shutdown();
  system("ros2 topic pub --once /forward_effort_controller/commands std_msgs/msg/Float64MultiArray 'data: [0,0,0]'");
  return 0;
}
