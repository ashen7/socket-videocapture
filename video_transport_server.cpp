/*
	服务器端：
  1、创建一个socket，用函数socket()； 
　　2、设置socket属性，用函数setsockopt(); * 可选 
　　3、绑定IP地址、端口等信息到socket上，用函数bind(); 
　　4、开启监听，用函数listen()； 
　　5、接收客户端上来的连接，用函数accept()； 
　　6、收发数据，用函数send()和recv()，或者read()和write(); 
　　7、关闭网络连接； 
　　8、关闭监听； 
*/
#include <errno.h>
#include <cstring>
#include <assert.h>   //断言

#include <unistd.h>   //这几个全是Linux socket的头文件
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include <iostream>
#include <string>     //string类模板的头文件
#include <memory>
#include <thread>
#include <atomic>
#include <vector>

#include <gflags/gflags.h> //google的glags和glog头文件
#include <glog/logging.h>
#include <opencv2/opencv.hpp>

#define IMG_WIDTH 640    //接收帧的宽
#define IMG_HEIGHT 480   //接收帧的高
#define BUFFER_SIZE IMG_WIDTH * IMG_HEIGHT * 3 / 32

DEFINE_int32(localhost_port, 8888, "localhost port");
DEFINE_string(localhost_ip, "10.209.149.204", "localhost ip");
std::atomic<bool> image_preprocessing_thread_stop_flag(false);
std::atomic<bool> face_detection_thread_stop_flag(false);

struct ReceiveBuffer {  
	char buffer[BUFFER_SIZE]; //把得到的32分之一的图片的数据存入这个结构体 再来赋值给一个mat对象 15行每行640*3个像素点  
	int is_frame_end;         //记录一下当前是否是一张图片的第32次了 也就是这张图片的最后一次发包 1表示不是 2表示是 最后count应该等于33
} receive_frame_data;
 
static void InitializeGlog(int argc, char* argv[]);                      //初始化glog日志
static int RunTcpServer(int32_t localhost_port, std::string localhost_ip); //TCP服务端
static int ReceiveFrame(int new_socket, cv::Mat& frame);                          //接收客户端发来的帧
static int ImagePreProcessing(const cv::Mat& source_image, cv::Mat& dst_image);
static int FaceDetection(const cv::Mat& source_image, cv::Mat& dst_image);

//windows是以unicode UTF-16来编码的 Linux是UTF-8 
int main(int argc, char* argv[]) {
    InitializeGlog(argc, argv);
    LOG(INFO) << "Start Google Logging!";

    //Run Server
    if (0 != RunTcpServer(FLAGS_localhost_port, FLAGS_localhost_ip)) {
		LOG(ERROR) << "run programming is fail, reason: " << strerror(errno);
    } else {
		LOG(INFO) << "run programming is successful!"; 
    }

    LOG(INFO) << "Close Google Logging!";
    google::ShutdownGoogleLogging();
	return 0;
}

void InitializeGlog(int argc, char *argv[]) {
    //初始化gflags 
    google::ParseCommandLineFlags(&argc, &argv, true);
    //初始化glog
    google::InitGoogleLogging(argv[0]);
    //设置日志输出目录 以及最低级别
    google::SetLogDestination(google::GLOG_INFO, "log/");
    //大于指定级别的日志输出到标准输出
    google::SetStderrLogging(google::GLOG_INFO);

    //设置stderr的颜色消息
    FLAGS_colorlogtostderr = true;
    //设置日志前缀是否应该添加到每行输出
    FLAGS_log_prefix = true;
    //设置可以缓冲日志的最大秒数 0表示实时输出日志
    FLAGS_logbufsecs = 0;
    //设置最大日志文件大小 MB
    FLAGS_max_log_size = 10;
}

int RunTcpServer(int32_t localhost_port, std::string localhost_ip) {
	//1. 创建套接字
	int server_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

	//2. 绑定套接字
	struct sockaddr_in server_addr;
	memset(&server_addr, 0, sizeof(struct sockaddr_in));   //每个字节都用0填充
	server_addr.sin_family = AF_INET;                      //使用IPv4地址
	server_addr.sin_addr.s_addr = inet_addr(localhost_ip.c_str());           //ip
	server_addr.sin_port = htons(localhost_port);                    //端口 host to network
	bind(server_socket, (struct sockaddr*)&server_addr, sizeof(struct sockaddr));
	//3. 监听状态
	listen(server_socket, 10);

	//4. 接收客户端请求
	struct sockaddr_in new_addr;
	socklen_t new_addr_size = sizeof(new_addr);
	int new_socket = accept(server_socket, (struct sockaddr*)&new_addr, &new_addr_size);
    LOG(INFO) << "等待一个新的客户端连接...";

    while (true) {
        //记录每一次程序 收包 加上 图像预处理 边缘检测 人脸识别的耗时
        double begin = static_cast<double>(cv::getTickCount());
        cv::Mat image(IMG_HEIGHT, IMG_WIDTH, CV_8UC3, cv::Scalar(0));
        if (0 != ReceiveFrame(new_socket, image)) {
            LOG(ERROR) << "receive frame from client is fail...";
            break;
        } else {
            //图像预处理
            //先翻转 水平方向翻转180度(1) 垂直180(0) 水平和垂直(-1)
            cv::flip(image, image, 1);
            cv::Mat preprocessing_destination_image;
            cv::Mat face_detection_destination_image;

            //开启两个线程跑
            //std::thread image_preprocessing_thread(ImagePreProcessing, std::cref(image), std::ref(preprocessing_destination_image));
            //std::thread face_detection_thread(FaceDetection, std::cref(image), std::ref(face_detection_destination_image));
            //image_preprocessing_thread.detach();
            //face_detection_thread.detach();
            //智能指针容器
            std::vector<std::shared_ptr<std::thread>> shared_thread_list;
            //创建容器后 第一时间为容器分配足够大的空间 避免后面扩容时重新分配内存
            shared_thread_list.reserve(5);
            shared_thread_list.push_back(std::make_shared<std::thread>(ImagePreProcessing, std::cref(image), std::ref(preprocessing_destination_image)));
            shared_thread_list.push_back(std::make_shared<std::thread>(FaceDetection, std::cref(image), std::ref(face_detection_destination_image)));
            shared_thread_list.at(0)->detach();
            shared_thread_list.at(1)->detach();
            
            //每次进来手动置0一下 不然直接跳过循环了 
            image_preprocessing_thread_stop_flag = false;
            face_detection_thread_stop_flag = false;
            //等待两个线程都运行完成
            while (!image_preprocessing_thread_stop_flag ||
                        !face_detection_thread_stop_flag) {
            }

            LOG(INFO) << "程序收包，图像预处理，边缘检测，人脸识别一次耗时：" << (static_cast<double>(cv::getTickCount()) - begin) / cv::getTickFrequency();
            cv::imshow("canny edge detection", preprocessing_destination_image);
            cv::imshow("haar feature face and eyes detection", face_detection_destination_image);
            //27就是按Esc的返回值
            if (27 == cv::waitKey(10)) {
                LOG(WARNING) << "按下ESC 正在退出程序...";
                break;
            }
        }
    }

	//关闭套接字
	close(server_socket);
	close(new_socket);

    return 0;
}


int ReceiveFrame(int new_socket, cv::Mat& image) {
	//大小就是32分之一的图片数据要的字节大小
	int need_receive_size = sizeof(receive_frame_data);
	int count = 0;

	//循环32次
	for (int packet_number = 0; packet_number < 32; packet_number++) {
		int current_receive_size = 0;
		int receive_size = 0;
		while (current_receive_size < need_receive_size) {
			//len得到实际读来的数据大小 如果错误就是-1
			receive_size = read(new_socket, (char*)&receive_frame_data, need_receive_size - current_receive_size);
			if (receive_size < 0)
				LOG(ERROR) << strerror(errno);
			
			//while循环一直到32分之一的数据全部写入结构体变量中
			current_receive_size += receive_size;
		}

		//count用来记录是否是一张图片的最后一次发送 
		count += receive_frame_data.is_frame_end;
		int current_packet_number = IMG_HEIGHT / 32 * packet_number;

		for (int current_line = 0; current_line < IMG_HEIGHT / 32; current_line++) {
            //记录当前已经赋值了多少行数据 
			int current_line_data = current_line * IMG_WIDTH * 3;
            //拿到每行帧数据的指针
			uchar* current_line_frame_data = image.ptr<uchar>(current_line + current_packet_number);
			for (int current_pixel = 0; current_pixel < IMG_WIDTH * 3; current_pixel++) {
				current_line_frame_data[current_pixel] = receive_frame_data.buffer[current_line_data + current_pixel];
			}
		}

		if (receive_frame_data.is_frame_end == 2) {
			if (count == 33) {
				return 0;
			} else {
                return -1;
			}
		}
	}
}

int ImagePreProcessing(const cv::Mat& source_image, cv::Mat& dst_image) {
    //灰度图
    cv::cvtColor(source_image, dst_image, CV_BGR2GRAY);
    //高斯滤波
    cv::GaussianBlur(dst_image, dst_image, cv::Size(5, 5), 0, 0, cv::BORDER_DEFAULT);
    //canny边缘检测 1.滤波平滑噪声 边缘检测算法基于图像强度的一阶和二阶微分操作 对噪声很敏感
    //2.利用已有的一阶偏导算子计算梯度 一般用sobel算子
    //3.非极大值抑制 寻找像素点局部的最大值 将非极大值点所对应的灰度值设置为背景像素点 
    //像素领域区域满足梯度值的局部最优质判断为该像素的边缘 对其余极大值相关信息进行抑制 
    //主要排除非边缘像素 保存候选图像边缘
    //4.滞后阈值算法 高于高阈值的认为是边缘 低于低阈值的认为非边缘 中间的如果和边缘相邻就认为是边缘
    int edge_threshold = 100;
    cv::Canny(dst_image, dst_image, edge_threshold, edge_threshold * 3, 3);

    image_preprocessing_thread_stop_flag = true;
    return 0;
}

int FaceDetection(const cv::Mat& source_image, cv::Mat& dst_image) {
    //基于Haar Feature的Cascade级联分类器
    cv::CascadeClassifier face_cascade;
    cv::CascadeClassifier eyes_cascade;
    
    cv::String face_cascade_name = "/home/yipeng/thirdlib_src/opencv-3.4.5/data/haarcascades/haarcascade_frontalface_alt.xml";
    cv::String eyes_cascade_name = "/home/yipeng/thirdlib_src/opencv-3.4.5/data/haarcascades/haarcascade_eye_tree_eyeglasses.xml";

    //加载.xml分类器文件  可以是Haar或LBP分类器
    if (!face_cascade.load(face_cascade_name)) {
        LOG(ERROR) << "failed to load face cascade!";
        face_detection_thread_stop_flag = true;
        return -1;
    }

    if (!eyes_cascade.load(eyes_cascade_name)) {
        LOG(ERROR) << "failed to load eyes cascade!";
        face_detection_thread_stop_flag = true;
        return -1;
    }
    
    //clone 或者 copyTo 不会影响原Mat数据 深拷  
    dst_image = source_image.clone();
    cv::Mat gray_image;
    //灰度图
    cv::cvtColor(source_image, gray_image, CV_BGR2GRAY);
    //直方图均衡化  使图像对比度均衡 和另一个改善图像对比度亮度函数一样 convertTo
    equalizeHist(gray_image, gray_image);
    std::vector<cv::Rect> faces_pointer;
    //执行检测  结果放入矩形列表中(检测到一个人脸 列表数量就是1, x, y, width, heihgt)
    face_cascade.detectMultiScale(gray_image, faces_pointer);
    
    for (int i = 0; i < faces_pointer.size(); i++) {
        //得到每个中心点
        cv::Point face_center(faces_pointer[i].x + faces_pointer[i].width / 2, 
                              faces_pointer[i].y + faces_pointer[i].height / 2);
        //画椭圆   中心点  封装的盒子大小  0-360度之间延伸 颜色 现款  类型
        cv::ellipse(dst_image, face_center, cv::Size(faces_pointer[i].width / 2, 
                                                     faces_pointer[i].height / 2), 
                                                     0, 0, 360, 
                                                     cv::Scalar(0, 0, 255), 4);
        //画矩形
        //cv::rectangle(dst_image, cv::Point(faces_pointer[i].x, faces_pointer[i].y),
        //              cv::Point(faces_pointer[i].x + faces_pointer[i].width, faces_pointer[i].y + faces_pointer[i].height), 
        //              cv::FILLED, cv::LINE_8);
        //得到脸部roi
        
        cv::Mat face_roi = gray_image(faces_pointer[i]);
        std::vector<cv::Rect> eyes_pointer;
        //执行检测
        eyes_cascade.detectMultiScale(face_roi, eyes_pointer);

        for (int j = 0; j < eyes_pointer.size(); j++) {
            //得到中心点
            cv::Point eyes_center(faces_pointer[i].x + eyes_pointer[j].x + eyes_pointer[j].width / 2, 
                                  faces_pointer[i].y + eyes_pointer[j].y + eyes_pointer[j].height / 2);
            int radius = cvRound(eyes_pointer[j].width + eyes_pointer[j].height * 0.25);
            cv::circle(dst_image, eyes_center, radius, cv::Scalar(0, 255, 0), 4);
        }
    }

    face_detection_thread_stop_flag = true;
    return 0;
}
