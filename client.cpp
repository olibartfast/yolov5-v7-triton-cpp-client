#include "Yolo.hpp"
#include "Triton.hpp"
#ifdef WRITE_FRAME
#include <opencv2/videoio.hpp>
#endif



static const std::string keys = 
    "{ model m | yolov7-tiny_onnx | model name of folder in triton }"
    "{ help h   | | Print help message. }"
    "{ video v | video.mp4 | video name}"
    "{ serverAddress  s  | localhost:8000 | Path to server address}"
    "{ verbose vb | false | Verbose mode, true or false}"
    "{ protocol p | http | Protocol type, grpc or http}"
    "{ labelsFile l | ../coco.names | path to  coco labels names}"
    "{ batch b | 1 | Batch size}";


int main(int argc, const char* argv[])
{
    cv::CommandLineParser parser(argc, argv, keys);
    if (parser.has("help")){
        parser.printMessage();
        return 0;  
    }

    std::cout << "Current path is " << std::filesystem::current_path() << '\n';
    std::string serverAddress = parser.get<std::string>("serverAddress");
    bool verbose = parser.get<bool>("verbose");
    std::string videoName;
    videoName = parser.get<std::string>("video");
    Triton::ProtocolType protocol = parser.get<std::string>("protocol") == "grpc" ? Triton::ProtocolType::GRPC : Triton::ProtocolType::HTTP;
    const size_t batch_size = parser.get<size_t>("batch");

    Triton::TritonModelInfo model_info;
    std::string preprocess_output_filename;
    std::string modelName = parser.get<std::string>("model");
    std::string modelVersion = "";
    std::string url(serverAddress);
    
    tc::Headers httpHeaders;

    const std::string fileName = parser.get<std::string>("labelsFile"); 

    std::cout << "Server address: " << serverAddress << std::endl;
    std::cout << "Video name: " << videoName << std::endl;
    std::cout << "Protocol:  " << parser.get<std::string>("protocol") << std::endl;
    std::cout << "Path to labels name:  " << fileName << std::endl;
    std::cout << "Model name:  " << modelName << std::endl;

    Triton::TritonClient tritonClient;
    tc::Error err;
    if (protocol == Triton::ProtocolType::HTTP)
    {
        err = tc::InferenceServerHttpClient::Create(
            &tritonClient.httpClient, url, verbose);
    }
    else
    {
        err = tc::InferenceServerGrpcClient::Create(
            &tritonClient.grpcClient, url, verbose);
    }
    if (!err.IsOk())
    {
        std::cerr << "error: unable to create client for inference: " << err
                  << std::endl;
        exit(1);
    }

    Triton::TritonModelInfo yoloModelInfo = Triton::setModel(batch_size);

    tc::InferInput *input;
    err = tc::InferInput::Create(
         &input, yoloModelInfo.input_name_, yoloModelInfo.shape_, yoloModelInfo.input_datatype_);
     if (!err.IsOk())
    {
         std::cerr << "unable to get input: " << err << std::endl;
         exit(1);
    }

    std::shared_ptr<tc::InferInput> input_ptr(input);

    std::vector<tc::InferInput *> inputs = {input_ptr.get()};
    std::vector<const tc::InferRequestedOutput *> outputs;

    for (auto output_name : yoloModelInfo.output_names_)
    {
        tc::InferRequestedOutput *output;
        err =
            tc::InferRequestedOutput::Create(&output, output_name);
        if (!err.IsOk())
        {
            std::cerr << "unable to get output: " << err << std::endl;
            exit(1);
        }
        else
            std::cout << "Created output " << output_name << std::endl;
        outputs.push_back(std::move(output));
    }

    tc::InferOptions options(modelName);
    options.model_version_ = modelVersion;

    cv::Mat frame;
    std::vector<uint8_t> input_data;
    std::vector<cv::Mat> frameBatch;
    cv::VideoCapture cap(videoName);

#ifdef WRITE_FRAME
    cv::VideoWriter outputVideo; 
    cv::Size S = cv::Size((int) cap.get(cv::CAP_PROP_FRAME_WIDTH),    // Acquire input size
                (int) cap.get(cv::CAP_PROP_FRAME_HEIGHT));
    int codec = cv::VideoWriter::fourcc('M', 'J', 'P', 'G');
    outputVideo.open("processed.avi", codec, cap.get(cv::CAP_PROP_FPS), S, true);    
    if (!outputVideo.isOpened())
    {
        std::cout  << "Could not open the output video for write: " << videoName << std::endl;
        return -1;
    }    
#endif       


    Yolo::coco_names = Yolo::readLabelNames(fileName);

    if(Yolo::coco_names.size() != Yolo::CLASS_NUM){
        std::cerr << "Wrong labels filename or wrong path to file: " << fileName << std::endl;
        exit(1);
    }

    while (cap.read(frame))
    {
        // Reset the input for new request.
        err = input_ptr->Reset();
        if (!err.IsOk())
        {
            std::cerr << "failed resetting input: " << err << std::endl;
            exit(1);
        }
        auto start = std::chrono::steady_clock::now();
        input_data = Yolo::Preprocess(
            frame, yoloModelInfo.input_format_, yoloModelInfo.type1_, yoloModelInfo.type3_,
            yoloModelInfo.input_c_ , cv::Size(yoloModelInfo.input_w_, yoloModelInfo.input_h_));
        
        err = input_ptr->AppendRaw(input_data);
        if (!err.IsOk())
        {
            std::cerr << "failed setting input: " << err << std::endl;
            exit(1);
        }
        tc::InferResult *result;
        std::unique_ptr<tc::InferResult> result_ptr;
        if (protocol == Triton::ProtocolType::HTTP)
        {
                err = tritonClient.httpClient->Infer(
            &result, options, inputs, outputs);
        }
        else
        {
            err = tritonClient.grpcClient->Infer(
                &result, options, inputs, outputs);
        }
        if (!err.IsOk())
        {
            std::cerr << "failed sending synchronous infer request: " << err
                    << std::endl;
            exit(1);
        }
        auto [infer_results, infer_shape] = Triton::getInferResults(result, batch_size, yoloModelInfo.output_names_, yoloModelInfo.max_batch_size_ != 0);
        result_ptr.reset(result);
        auto end = std::chrono::steady_clock::now();
        auto diff = end - start;
        std::cout << "Infer time: " << std::chrono::duration<double, std::milli>(diff).count() << " ms" << std::endl;

        std::vector<Yolo::Detection> detections = Yolo::postprocess(cv::Size(frame.cols, frame.rows),
            infer_results, infer_shape);


#if defined(SHOW_FRAME) || defined(WRITE_FRAME)
        for (auto&& detection : detections)
        {
            cv::rectangle(frame, detection.bbox, cv::Scalar(255, 0,0), 2);
            cv::putText(frame, Yolo::coco_names[detection.class_id], 
                cv::Point(detection.bbox.x, detection.bbox.y - 1), cv::FONT_HERSHEY_PLAIN, 1.2, cv::Scalar(0xFF, 0xFF, 0xFF), 2);
        }           
#endif
#ifdef SHOW_FRAME
        cv::imshow("video feed ", frame);
        cv::waitKey(1);  
#endif      
#ifdef WRITE_FRAME
        outputVideo.write(frame);
#endif  

    }

    return 0;
}