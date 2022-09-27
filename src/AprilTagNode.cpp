// ros
#include <rclcpp/rclcpp.hpp>
#include <sensor_msgs/msg/camera_info.hpp>
#include <sensor_msgs/msg/image.hpp>
#include <apriltag_msgs/msg/april_tag_detection.hpp>
#include <apriltag_msgs/msg/april_tag_detection_array.hpp>
#include <tf2_ros/transform_broadcaster.h>
#include <image_transport/camera_subscriber.hpp>
#include <image_transport/image_transport.hpp>
#include <cv_bridge/cv_bridge.h>
#include <rclcpp_components/register_node_macro.hpp>

// apriltag
#include <apriltag.h>
#include "tag_functions.hpp"
// for precise pose estimation
#include <apriltag_pose.h>

#include <Eigen/Dense>


#define IF(N, V) if (assign_check(parameter, N, V)) continue;

template<typename T>
void assign(const rclcpp::Parameter& parameter, T& var)
{
    var = parameter.get_value<T>();
}

template<typename T>
void assign(const rclcpp::Parameter& parameter, std::atomic<T>& var)
{
    var = parameter.get_value<T>();
}

template<typename T>
bool assign_check(const rclcpp::Parameter& parameter, const std::string& name, T& var)
{
    if (parameter.get_name() == name) {
        assign(parameter, var);
        return true;
    }
    return false;
}


typedef Eigen::Matrix<double, 3, 3, Eigen::RowMajor> Mat3;

rcl_interfaces::msg::ParameterDescriptor
descr(const std::string& description, const bool& read_only = false)
{
    rcl_interfaces::msg::ParameterDescriptor descr;

    descr.description = description;
    descr.read_only = read_only;

    return descr;
}

void getPose(apriltag_detection_t* det,
             const Mat3& K,
             const Mat3& Pinv,
             geometry_msgs::msg::Transform& t,
             const double size,
             const bool z_up,
             const bool refine_pose)
{
    // compute extrinsic camera parameter
    Mat3 R;
    Eigen::Vector3d tt;
    if(refine_pose) {
        // by using estimate_tag_pose() - high precision
        // First create an apriltag_detection_info_t struct using your known parameters
        apriltag_detection_info_t info;
        info.det = det;
        info.tagsize = size;
        info.fx = K(0,0);
        info.fy = K(1,1);
        info.cx = K(0,2);
        info.cy = K(1,2);
        // Then call estimate_tag_pose
        apriltag_pose_t pose;
        //double err =
        estimate_tag_pose(&info, &pose);
        R = Eigen::Map<Mat3>(pose.R->data);
        tt= Eigen::Map<Eigen::Vector3d>(pose.t->data);
    } else {
        // by using the homography - low precision
        // https://dsp.stackexchange.com/a/2737/31703
        // H = K * T  =>  T = K^(-1) * H
        const Mat3 T = Pinv * Eigen::Map<const Mat3>((det->H)->data);
        
        R.col(0) = T.col(0).normalized();
        R.col(1) = T.col(1).normalized();
        R.col(2) = R.col(0).cross(R.col(1));

        // the corner coordinates of the tag in the canonical frame are (+/-1, +/-1)
        // hence the scale is half of the edge size
        tt = T.rightCols<1>() / ((T.col(0).norm() + T.col(0).norm())/2.0) * (size/2.0);
    }

    if(z_up) {
        // rotate by half rotation about x-axis
        R.col(1) *= -1;
        R.col(2) *= -1;
    }

    const Eigen::Quaterniond q(R);

    t.translation.x = tt.x();
    t.translation.y = tt.y();
    t.translation.z = tt.z();
    t.rotation.w = q.w();
    t.rotation.x = q.x();
    t.rotation.y = q.y();
    t.rotation.z = q.z();
}


class AprilTagNode : public rclcpp::Node {
public:
    AprilTagNode(const rclcpp::NodeOptions& options);

    ~AprilTagNode() override;

private:
    const OnSetParametersCallbackHandle::SharedPtr cb_parameter;

    apriltag_family_t* tf;
    apriltag_detector_t* const td;

    // parameter
    std::mutex mutex;
    double tag_edge_size;
    std::atomic<int> max_hamming;
    std::atomic<bool> profile;
    std::unordered_map<int, std::string> tag_frames;
    std::unordered_map<int, double> tag_sizes;
    std::atomic<bool> z_up;
    std::atomic<bool> refine_pose;

    std::function<void(apriltag_family_t*)> tf_destructor;

    const image_transport::CameraSubscriber sub_cam;
    const rclcpp::Publisher<apriltag_msgs::msg::AprilTagDetectionArray>::SharedPtr pub_detections;
    tf2_ros::TransformBroadcaster tf_broadcaster;

    void onCamera(const sensor_msgs::msg::Image::ConstSharedPtr& msg_img, const sensor_msgs::msg::CameraInfo::ConstSharedPtr& msg_ci);

    rcl_interfaces::msg::SetParametersResult onParameter(const std::vector<rclcpp::Parameter>& parameters);
};

RCLCPP_COMPONENTS_REGISTER_NODE(AprilTagNode)


AprilTagNode::AprilTagNode(const rclcpp::NodeOptions& options)
  : Node("apriltag", options),
    // parameter
    cb_parameter(add_on_set_parameters_callback(std::bind(&AprilTagNode::onParameter, this, std::placeholders::_1))),
    td(apriltag_detector_create()),
    // topics
    sub_cam(image_transport::create_camera_subscription(this, "image_rect", std::bind(&AprilTagNode::onCamera, this, std::placeholders::_1, std::placeholders::_2), declare_parameter("image_transport", "raw", descr({}, true)), rmw_qos_profile_sensor_data)),
    pub_detections(create_publisher<apriltag_msgs::msg::AprilTagDetectionArray>("detections", rclcpp::QoS(1))),
    tf_broadcaster(this)
{
    // read-only parameters
    const std::string tag_family = declare_parameter("family", "36h11", descr("tag family", true));
    tag_edge_size = declare_parameter("size", 1.0, descr("default tag size", true));

    // get tag names, IDs and sizes
    const auto ids =    declare_parameter("tag.ids", std::vector<int64_t>{}, descr("tag ids", true));
    const auto frames = declare_parameter("tag.frames", std::vector<std::string>{}, descr("tag frame names per id", true));
    const auto sizes =  declare_parameter("tag.sizes", std::vector<double>{}, descr("tag sizes per id", true));

    // detector parameters in "detector" namespace
    declare_parameter("detector.threads", td->nthreads, descr("number of threads"));
    declare_parameter("detector.decimate", td->quad_decimate, descr("decimate resolution for quad detection"));
    declare_parameter("detector.blur", td->quad_sigma, descr("sigma of Gaussian blur for quad detection"));
    declare_parameter<bool>("detector.refine", td->refine_edges, descr("snap to strong gradients"));
    declare_parameter("detector.sharpening", td->decode_sharpening, descr("sharpening of decoded images"));
    declare_parameter<bool>("detector.debug", td->debug, descr("write additional debugging images to working directory"));

    declare_parameter<int>("max_hamming", 0, descr("reject detections with more corrected bits than allowed"));
    declare_parameter("profile", false, descr("print profiling information to stdout"));
    declare_parameter<bool>("z_up", true, descr("let the z axis of the tag frame point up"));
    declare_parameter<bool>("refine-pose", false, descr("increase the accuracy of the extracted pose"));

    if(!frames.empty()) {
        if(ids.size()!=frames.size()) {
            throw std::runtime_error("Number of tag ids ("+std::to_string(ids.size())+") and frames ("+std::to_string(frames.size())+") mismatch!");
        }
        for(size_t i = 0; i<ids.size(); i++) { tag_frames[ids[i]] = frames[i]; }
    }

    if(!sizes.empty()) {
        // use tag specific size
        if(ids.size()!=sizes.size()) {
            throw std::runtime_error("Number of tag ids ("+std::to_string(ids.size())+") and sizes ("+std::to_string(sizes.size())+") mismatch!");
        }
        for(size_t i = 0; i<ids.size(); i++) { tag_sizes[ids[i]] = sizes[i]; }
    }

    if(tag_create.count(tag_family)) {
        tf = tag_create.at(tag_family)();
        tf_destructor = tag_destroy.at(tag_family);
        apriltag_detector_add_family(td, tf);
    }
    else {
        throw std::runtime_error("Unsupported tag family: "+tag_family);
    }
}

AprilTagNode::~AprilTagNode() {
    apriltag_detector_destroy(td);
    tf_destructor(tf);
}

void AprilTagNode::onCamera(const sensor_msgs::msg::Image::ConstSharedPtr& msg_img,
                            const sensor_msgs::msg::CameraInfo::ConstSharedPtr& msg_ci)
{
    // precompute inverse projection matrix
    const Mat3 Pinv = Eigen::Map<const Eigen::Matrix<double, 3, 4, Eigen::RowMajor>>(msg_ci->p.data()).leftCols<3>().inverse();

    // convert to 8bit monochrome image
    const cv::Mat img_uint8 = cv_bridge::toCvShare(msg_img, "mono8")->image;

    image_u8_t im{img_uint8.cols, img_uint8.rows, img_uint8.cols, img_uint8.data};

    // detect tags
    mutex.lock();
    zarray_t* detections = apriltag_detector_detect(td, &im);
    mutex.unlock();

    if (profile)
        timeprofile_display(td->tp);

    apriltag_msgs::msg::AprilTagDetectionArray msg_detections;
    msg_detections.header = msg_img->header;

    std::vector<geometry_msgs::msg::TransformStamped> tfs;

    for (int i = 0; i < zarray_size(detections); i++) {
        apriltag_detection_t* det;
        zarray_get(detections, i, &det);

        // ignore untracked tags
        if(!tag_frames.empty() && !tag_frames.count(det->id)) { continue; }

        // reject detections with more corrected bits than allowed
        if(det->hamming>max_hamming) { continue; }

        // detection
        apriltag_msgs::msg::AprilTagDetection msg_detection;
        msg_detection.family = std::string(det->family->name);
        msg_detection.id = det->id;
        msg_detection.hamming = det->hamming;
        msg_detection.decision_margin = det->decision_margin;
        msg_detection.centre.x = det->c[0];
        msg_detection.centre.y = det->c[1];
        std::memcpy(msg_detection.corners.data(), det->p, sizeof(double)*8);
        std::memcpy(msg_detection.homography.data(), det->H->data, sizeof(double)*9);
        msg_detections.detections.push_back(msg_detection);

        // 3D orientation and position
        geometry_msgs::msg::TransformStamped tf;
        tf.header = msg_img->header;
        // set child frame name by generic tag name or configured tag name
        tf.child_frame_id = tag_frames.count(det->id) ? tag_frames.at(det->id) : std::string(det->family->name)+":"+std::to_string(det->id);
        getPose(det, Eigen::Map<const Mat3>(msg_ci->k.data()), Pinv, tf.transform, tag_sizes.count(det->id) ? tag_sizes.at(det->id) : tag_edge_size, z_up, refine_pose);

        tfs.push_back(tf);
    }

    pub_detections->publish(msg_detections);
    tf_broadcaster.sendTransform(tfs);

    apriltag_detections_destroy(detections);
}

rcl_interfaces::msg::SetParametersResult
AprilTagNode::onParameter(const std::vector<rclcpp::Parameter>& parameters)
{
    rcl_interfaces::msg::SetParametersResult result;

    mutex.lock();

    for (const rclcpp::Parameter& parameter : parameters) {
        RCLCPP_DEBUG_STREAM(get_logger(), "setting: " << parameter);

        IF("detector.threads", td->nthreads)
        IF("detector.decimate", td->quad_decimate)
        IF("detector.blur", td->quad_sigma)
        IF("detector.refine", td->refine_edges)
        IF("detector.sharpening", td->decode_sharpening)
        IF("detector.debug", td->debug)
        IF("max_hamming", max_hamming)
        IF("profile", profile)
        IF("z_up", z_up)
        IF("refine-pose", refine_pose)
    }

    mutex.unlock();

    result.successful = true;

    return result;
}
