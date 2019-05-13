#include "zivid_ros_wrapper/zivid_ros_wrapper.hpp"

#include <sensor_msgs/point_cloud2_iterator.h>
#include <dynamic_reconfigure/config_tools.h>

#include "zivid_ros_wrapper_gen.hpp"
#include <zivid_services_generated.h>

#include <condition_variable>
#include <csignal>

#include <iostream>
#include <sstream>

zivid_ros_wrapper::ZividRosWrapper::ZividRosWrapper(void) :
    camera_reconfigure_handler_m("~/camera_config"),
    camera_reconfigure_server_m(camera_reconfigure_handler_m)
{
    init();
}

zivid_ros_wrapper::ZividRosWrapper::~ZividRosWrapper(void)
{
    disconnect();
}

void zivid_ros_wrapper::ZividRosWrapper::init(void)
{
    camera_mode_m = 0;

    ros::NodeHandle nh;
    ros::NodeHandle priv("~");

    //First setup necessary parameters
    std::string serial_number = "";
    priv.param<std::string>("serial_number", serial_number, "");

    bool test_mode_enabled;
    nh.param<bool>("zivid_test_mode_enabled", test_mode_enabled, false);
    std::string test_mode_file;
    if(test_mode_enabled)
    {
        nh.param<std::string>("zivid_test_mode_file", test_mode_file, "test.zdf");
        ROS_INFO("Using test file %s", test_mode_file.c_str());
    }

    ROS_INFO("Connecting to zivid camera %s%s", serial_number.c_str(), test_mode_enabled ? " in test mode" : "");

    if(!test_mode_enabled)
    {
        if(serial_number == "")
            camera_m = zivid_m.connectCamera();
        else if(serial_number.find("sn") == 0)
            camera_m = zivid_m.connectCamera(Zivid::SerialNumber(serial_number.substr(2)));
        else
            ROS_ERROR("The format of the serial number is not recognized. It should start with sn in ROS because of ROS param conversion problems");
    }
    else
    {
        camera_m = zivid_m.createFileCamera(test_mode_file);
    }

    ROS_INFO("Connected to zivid camera: %s", camera_m.toString().c_str());
    camera_m.setFrameCallback(boost::bind(&zivid_ros_wrapper::ZividRosWrapper::frameCallbackFunction, this, _1));

    ROS_INFO("Setting up reconfigurable params");
    int added_settings = 1;
    newSettings("settings");
    if(priv.hasParam("settings_names"))
    {
        std::string settings_names;
        priv.getParam("settings_names", settings_names);

        int start_pos = 0;
        int end_pos = 0;
        while(start_pos < settings_names.size())
        {
            end_pos = settings_names.find(";", start_pos);
            if(end_pos < 0)
                end_pos = settings_names.size();

            std::string settings_name = settings_names.substr(start_pos, (end_pos - start_pos));
            newSettings(settings_name);
            added_settings++;

            start_pos = end_pos + 1;
        }
    }

    camera_reconfigure_server_m.setCallback(boost::bind(&zivid_ros_wrapper::ZividRosWrapper::cameraReconfigureCallback, this, _1, _2));

    ROS_INFO("Registering pointcloud live feed at %s", "pointcloud");
    pointcloud_pub_m = priv.advertise<sensor_msgs::PointCloud2>("pointcloud", 1);

    ROS_INFO("Registering pointcloud capture service at %s", "capture");
    boost::function<bool(zivid_ros_wrapper::Capture::Request&, zivid_ros_wrapper::Capture::Response&)> capture_callback_func
        = boost::bind(&zivid_ros_wrapper::ZividRosWrapper::captureServiceHandler, this, _1, _2);
    capture_service_m = priv.advertiseService("capture", capture_callback_func);

    ROS_INFO("Registering pointcloud hdr capture service at %s", "hdr");
    boost::function<bool(zivid_ros_wrapper::HDR::Request&, zivid_ros_wrapper::HDR::Response&)> hdr_callback_func
        = boost::bind(&zivid_ros_wrapper::ZividRosWrapper::hdrCaptureServiceHandler, this, _1, _2);
    hdr_service_m = priv.advertiseService("hdr", hdr_callback_func);

    setupStatusServers(priv, camera_m, generated_servers_m);

    ROS_INFO("Registering zivid info service at %s", "zivid_info");
    boost::function<bool(zivid_ros_wrapper::ZividInfo::Request&, zivid_ros_wrapper::ZividInfo::Response&)> zivid_info_callback_func
        = boost::bind(&zivid_ros_wrapper::ZividRosWrapper::zividInfoServiceHandler, this, _1, _2);
    zivid_info_service_m = priv.advertiseService("zivid_info", zivid_info_callback_func);
}

void zivid_ros_wrapper::ZividRosWrapper::disconnect(void)
{
    ROS_INFO("Disconecting zivid camera");
    if(camera_mode_m == ZividCamera_Live)
        camera_m.stopLive();
}

sensor_msgs::PointField createPointField(std::string name, uint32_t offset, uint8_t datatype, uint32_t count)
{
    sensor_msgs::PointField point_field;

    point_field.name = name;
    point_field.offset = offset;
    point_field.datatype = datatype;
    point_field.count = count;

    return point_field;
}

sensor_msgs::PointCloud2 zivid_ros_wrapper::ZividRosWrapper::zividFrameToPointCloud2(const Zivid::Frame& frame)
{
    Zivid::PointCloud point_cloud = frame.getPointCloud();
    sensor_msgs::PointCloud2 pointcloud_msg;

    //Setup header values
    pointcloud_msg.header.seq = frame_id_m++;
    pointcloud_msg.header.stamp = ros::Time::now();
    pointcloud_msg.header.frame_id = "zividsensor";

    //Now copy the point cloud information.
    pointcloud_msg.height = point_cloud.height();
    pointcloud_msg.width = point_cloud.width();
    pointcloud_msg.is_bigendian = false;
    pointcloud_msg.point_step = sizeof(Zivid::Point);
    pointcloud_msg.row_step = pointcloud_msg.point_step * pointcloud_msg.width;
    pointcloud_msg.is_dense = false;


    pointcloud_msg.fields.push_back(createPointField("x", 8, 7, 1));
    pointcloud_msg.fields.push_back(createPointField("y", 0, 7, 1));
    pointcloud_msg.fields.push_back(createPointField("z", 4, 7, 1));
    pointcloud_msg.fields.push_back(createPointField("c", 12, 7, 1));
    pointcloud_msg.fields.push_back(createPointField("rgba", 16, 7, 1));

    pointcloud_msg.data = std::vector<uint8_t>((uint8_t*)point_cloud.dataPtr(), (uint8_t*)(point_cloud.dataPtr() + point_cloud.size()));

    #pragma omp parallel for
    for(std::size_t i=0; i<point_cloud.size(); i++)
    {
        uint8_t* point_ptr = &(pointcloud_msg.data[i * sizeof(Zivid::Point)]);
        float* x_ptr = (float*) &(point_ptr[pointcloud_msg.fields[0].offset]);
        float* y_ptr = (float*) &(point_ptr[pointcloud_msg.fields[1].offset]);
        float* z_ptr = (float*) &(point_ptr[pointcloud_msg.fields[2].offset]);

        //TODO this rotation should actually be fixed in tf, even though it probably is slightly slower.
        *x_ptr *= 0.001f;
        *z_ptr *= - 0.001f;
        *y_ptr *= - 0.001f;
    }

    return pointcloud_msg;
}

void zivid_ros_wrapper::ZividRosWrapper::newSettings(const std::string& name)
{
    dynamic_reconfigure_settings_list_m.push_back(zivid_ros_wrapper::ZividRosWrapper::DynamicReconfigureSettings());
    DynamicReconfigureSettings& reconfigure_settings = dynamic_reconfigure_settings_list_m[dynamic_reconfigure_settings_list_m.size() - 1];

    reconfigure_settings.node_handle = ros::NodeHandle("~/"+name);
    reconfigure_settings.reconfigure_server =
        std::shared_ptr<dynamic_reconfigure::Server<zivid_ros_wrapper::ZividSettingsConfig>> (
            new dynamic_reconfigure::Server<zivid_ros_wrapper::ZividSettingsConfig>(reconfigure_settings.node_handle)
        );
    reconfigure_settings.reconfigure_server->setCallback(
        boost::bind(&zivid_ros_wrapper::ZividRosWrapper::settingsReconfigureCallback, this, _1, _2, name)
    );
    reconfigure_settings.name = name;
    reconfigure_settings.settings = Zivid::Settings{};
}

void zivid_ros_wrapper::ZividRosWrapper::removeSettings(const std::string& name)
{
    for(int i=0; i<dynamic_reconfigure_settings_list_m.size(); i++)
    {
        DynamicReconfigureSettings& reconfigure_settings = dynamic_reconfigure_settings_list_m[i];
        if(reconfigure_settings.name == name)
        {
            dynamic_reconfigure_settings_list_m.erase(dynamic_reconfigure_settings_list_m.begin() + i);
            break;
        }
    }
}

void zivid_ros_wrapper::ZividRosWrapper::frameCallbackFunction(const Zivid::Frame& frame)
{
    //Prepare message
    sensor_msgs::PointCloud2 pointcloud_msg = zividFrameToPointCloud2(frame);

    pointcloud_pub_m.publish(pointcloud_msg);
}


void zivid_ros_wrapper::ZividRosWrapper::settingsReconfigureCallback(zivid_ros_wrapper::ZividSettingsConfig &config, uint32_t /*level*/, const std::string& name)
{
    ROS_INFO("Running settings dynamic reconfiguration of %s", name.c_str());

    for(int i=0; i<dynamic_reconfigure_settings_list_m.size(); i++)
    {
        DynamicReconfigureSettings& reconfigure_settings = dynamic_reconfigure_settings_list_m[i];
        if(reconfigure_settings.name == name)
        {
            ROS_INFO("You updated setting %s", reconfigure_settings.name.c_str());

            dynamic_reconfigure::Config msg;
            config.__toMessage__(msg);

            Zivid::Settings& settings = reconfigure_settings.settings;
            settings.traverseValues([msg, &settings](auto &s)
            {
                using SettingType = std::remove_reference_t<decltype(s)>;
                std::string config_path = convertSettingsPathToConfigPath(s.path);
                settings.set(SettingType(getConfigValueFromString<typename SettingType::ValueType>(config_path, msg)));
            });
            if(reconfigure_settings.name == "settings")
                camera_m.setSettings(settings);
        }
    }
}

void zivid_ros_wrapper::ZividRosWrapper::cameraReconfigureCallback(zivid_ros_wrapper::ZividCameraConfig &config, uint32_t /*level*/)
{
    ROS_INFO("Running camera dynamic reconfiguration");
    //Update ros node settings
    configureCameraMode(config.camera_mode);
}

void zivid_ros_wrapper::ZividRosWrapper::configureCameraMode(int camera_mode)
{
    //Only perform this action if the value have actually changed
    if(camera_mode != camera_mode_m)
    {
        ROS_INFO("Setting camera from mode %d to %d\n", camera_mode_m, camera_mode);
        //Start by exiting previous state
        if(camera_mode_m == ZividCamera_Live)
        {
            camera_m.stopLive();
        }
        else if(camera_mode_m == ZividCamera_Capture)
        {}

        //Then setup the next state
        if(camera_mode == ZividCamera_Live)
        {
            camera_m.startLive();
        }

        camera_mode_m = camera_mode;
    }
}

bool zivid_ros_wrapper::ZividRosWrapper::captureServiceHandler(zivid_ros_wrapper::Capture::Request& /* req */, zivid_ros_wrapper::Capture::Response& res)
{
    ROS_INFO("Got a service request to publish point cloud. Capturing point cloud from camera");

    if(camera_mode_m == ZividCamera_Capture)
    {
        Zivid::Frame frame = camera_m.capture();
        sensor_msgs::PointCloud2 pointcloud_msg = zividFrameToPointCloud2(frame);

        res.pointcloud = pointcloud_msg;
        return true;
    }
    else
    {
        if(camera_mode_m != ZividCamera_Capture)
        {
            ROS_ERROR("Unable to obtain capture because ros wrapper was not set to \"capture\" mode");
        }
        else
        {
            ROS_ERROR("Unable to obtain pointcloud as camera is not connected");
        }
        return false;
    }
}

bool zivid_ros_wrapper::ZividRosWrapper::hdrCaptureServiceHandler(zivid_ros_wrapper::HDR::Request& req, zivid_ros_wrapper::HDR::Response& res)
{
    ROS_INFO("Got a service request to publish an HDR point cloud.");

    if(camera_mode_m == ZividCamera_Capture)
    {
        Zivid::Settings default_settings = camera_m.settings();
        std::vector<Zivid::Frame> hdr_frames;

        for(int i=0; i<req.iris_settings.size(); i++)
        {
            camera_m << Zivid::Settings::Iris{ req.iris_settings[i] };
            hdr_frames.emplace_back(camera_m.capture());
        }

        auto hdr_frame = Zivid::HDR::combineFrames(begin(hdr_frames), end(hdr_frames));
        sensor_msgs::PointCloud2 pointcloud_msg = zividFrameToPointCloud2(hdr_frame);
        res.pointcloud = pointcloud_msg;

        camera_m.setSettings(default_settings);

        return true;
    }
    else
    {
        ROS_ERROR("Unable to publish HDR images because camera is not in capture mode.");
    }
    return false;
}

bool zivid_ros_wrapper::ZividRosWrapper::zividInfoServiceHandler(zivid_ros_wrapper::ZividInfo::Request&, zivid_ros_wrapper::ZividInfo::Response& res)
{
    res.model_name = camera_m.modelName();
    res.camera_revision = camera_m.revision().toString();
    res.serial_number = camera_m.serialNumber().toString();
    res.firmware_version = camera_m.firmwareVersion();

    return true;
}