#ifndef ZIVID_CAMERA_H
#define ZIVID_CAMERA_H

#include <ros/ros.h>

#include <zivid_camera/CaptureFrameConfig.h>
//#include <zivid_camera/ZividCameraConfig.h>
#include <zivid_camera/CaptureGeneralConfig.h>
#include <zivid_camera/Capture.h>
#include <zivid_camera/CameraInfo.h>

#include <sensor_msgs/PointCloud2.h>
#include <sensor_msgs/Image.h>

#include <image_transport/image_transport.h>

#include <dynamic_reconfigure/server.h>

#include <Zivid/Application.h>
#include <Zivid/Camera.h>
#include <Zivid/Settings.h>
#include <Zivid/Version.h>

namespace zivid_camera
{
class ZividCamera
{
public:
  ZividCamera();
  ~ZividCamera();

private:
  void frameCallbackFunction(Zivid::Frame frame);
  void settingsReconfigureCallback(zivid_camera::CaptureFrameConfig& config, uint32_t level, const std::string& name);
  void newSettings(const std::string& name);
  // void cameraReconfigureCallback(zivid_camera::ZividCameraConfig& config, uint32_t level);
  void captureGeneralReconfigureCb(zivid_camera::CaptureGeneralConfig& config, uint32_t level);
  // void configureCameraMode(int camera_mode);
  bool captureServiceHandler(zivid_camera::Capture::Request& req, zivid_camera::Capture::Response& res);
  bool cameraInfoServiceHandler(zivid_camera::CameraInfo::Request& req, zivid_camera::CameraInfo::Response& res);

  void publishFrame(Zivid::Frame&& frame);
  sensor_msgs::PointCloud2 frameToPointCloud2(const Zivid::Frame& frame);
  sensor_msgs::Image frameToColorImage(const Zivid::Frame& frame);
  sensor_msgs::Image frameToDepthImage(const Zivid::Frame& frame);
  sensor_msgs::Image createNewImage(const Zivid::PointCloud& point_cloud, const std::string& encoding,
                                    std::size_t step);

  struct DynamicReconfigureFrameConfig
  {
    using CfgType = zivid_camera::CaptureFrameConfig;
    std::string name;
    std::shared_ptr<dynamic_reconfigure::Server<CfgType>> reconfigure_server;
    CfgType config;
  };

  // int camera_mode_;
  int frame_id_;
  ros::NodeHandle nh_;
  ros::NodeHandle priv_;

  // ros::NodeHandle camera_reconfigure_handler_;
  // dynamic_reconfigure::Server<zivid_camera::ZividCameraConfig> camera_reconfigure_server_;
  ros::NodeHandle capture_general_dynreconfig_node_;
  dynamic_reconfigure::Server<zivid_camera::CaptureGeneralConfig> capture_general_dynreconfig_server_;
  zivid_camera::CaptureGeneralConfig currentCaptureGeneralConfig_;

  ros::Publisher pointcloud_pub_;
  image_transport::ImageTransport image_transport_;
  image_transport::Publisher color_image_publisher_;
  image_transport::Publisher depth_image_publisher_;

  ros::ServiceServer capture_service_;
  std::vector<ros::ServiceServer> generated_servers_;
  ros::ServiceServer zivid_info_service_;
  std::vector<DynamicReconfigureFrameConfig> frame_configs_;
  Zivid::Application zivid_;
  Zivid::Camera camera_;
};
}  // namespace zivid_camera

#endif
