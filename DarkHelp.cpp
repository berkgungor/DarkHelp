/* DarkHelp - C++ helper class for Darknet's C API.
 * Copyright 2019 Stephane Charette <stephanecharette@gmail.com>
 * MIT license applies.  See "license.txt" for details.
 * $Id$
 */

#include <DarkHelp.hpp>
#include <fstream>
#include <cmath>


DarkHelp::~DarkHelp()
{
	return;
}


DarkHelp::DarkHelp(const std::string & cfg_filename, const std::string & weights_filename, const std::string & names_filename)
{
	if (cfg_filename.empty())
	{
		/// @thow std::invalid_argument if the configuration filename is empty.
		throw std::invalid_argument("darknet configuration filename cannot be empty");
	}
	if (weights_filename.empty())
	{
		/// @thow std::invalid_argument if the weights filename is empty.
		throw std::invalid_argument("darknet weights filename cannot be empty");
	}

	const auto t1 = std::chrono::high_resolution_clock::now();
	net = load_network_custom(const_cast<char*>(cfg_filename.c_str()), const_cast<char*>(weights_filename.c_str()), 1, 1);
	const auto t2 = std::chrono::high_resolution_clock::now();
	duration = t2 - t1;

	if (net == nullptr)
	{
		/// @throw std::runtime_error if the call to darknet's load_network_custom() has failed.
		throw std::runtime_error("darknet failed to load the configuration, the weights, or both");
	}

	// what do these 2 calls do?
	fuse_conv_batchnorm(*net);
	calculate_binary_weights(*net);

	// pick some reasonable default values
	threshold							= 0.5f;
	hierchy_threshold					= 0.5f;
	non_maximal_suppression_threshold	= 0.45f;
	annotation_colour					= cv::Scalar(255, 0, 255);
	annotation_font_scale				= 0.5;
	annotation_font_thickness			= 1;

	if (not names_filename.empty())
	{
		std::ifstream ifs(names_filename);
		std::string line;
		while (std::getline(ifs, line))
		{
			if (line.empty())
			{
				break;
			}
			names.push_back(line);
		}
	}

	return;
}


DarkHelp::PredictionResults DarkHelp::predict(const std::string & image_filename, const float new_threshold)
{
	cv::Mat mat = cv::imread(image_filename);
	if (mat.empty())
	{
		/// @throw std::invalid_argument if the image failed to load.
		throw std::invalid_argument("failed to load image \"" + image_filename + "\"");
	}

	return predict(mat, new_threshold);
}


DarkHelp::PredictionResults DarkHelp::predict(cv::Mat mat, const float new_threshold)
{
	if (mat.empty())
	{
		/// @throw std::invalid_argument if the image is empty.
		throw std::invalid_argument("cannot predict with an empty OpenCV image");
	}

	original_image = mat;

	return predict(new_threshold);
}


DarkHelp::PredictionResults DarkHelp::predict(image img, const float new_threshold)
{
	/* This is inefficient since we eventually need a Darknet "image", but we're going to convert the image back to a
	 * OpenCV-format cv::Mat.  This allows the DarkHelp object to be more consistent with the way it handles images.
	 */
	cv::Mat mat = convert_darknet_image_to_opencv_mat(img);
	if (mat.empty())
	{
		/// @throw std::invalid_argument if the image is empty.
		throw std::invalid_argument("image is empty or has failed to convert from Darknet's 'image' format");
	}

	return predict(mat, new_threshold);
}


cv::Mat DarkHelp::annotate(const float new_threshold, const bool include_duraton)
{
	if (original_image.empty())
	{
		/// @throw std::logic_error if an attempt is made to annotate an empty image
		throw std::logic_error("cannot annotate an empty image; must call predict() first");
	}

	if (new_threshold >= 0.0)
	{
		threshold = new_threshold;
	}

	annotated_image = original_image.clone();

	const auto font_face = cv::FONT_HERSHEY_SIMPLEX;

	for (const auto & pred : prediction_results)
	{
		if (pred.probability >= threshold)
		{
			std::cout << "class id=" << pred.class_id << ", probability=" << pred.probability << ", point=(" << pred.rect.x << "," << pred.rect.y << ")" << std::endl;
			cv::rectangle(annotated_image, pred.rect, annotation_colour, 2);

			const cv::Size text_size = cv::getTextSize(pred.name, font_face, annotation_font_scale, annotation_font_thickness, nullptr);

			cv::Rect r(cv::Point(pred.rect.x - 1, pred.rect.y - text_size.height), cv::Size(text_size.width + 2, text_size.height + 2));
			cv::rectangle(annotated_image, r, annotation_colour, CV_FILLED);
			cv::putText(annotated_image, pred.name, cv::Point(r.x + 1, r.y + text_size.height), font_face, annotation_font_scale, cv::Scalar(0,0,0), annotation_font_thickness, CV_AA);
		}
	}

	if (include_duraton)
	{
		const std::string str		= duration_string();
		const cv::Size text_size	= cv::getTextSize(str, font_face, annotation_font_scale, annotation_font_thickness, nullptr);

		cv::Rect r(cv::Point(2, 2), cv::Size(text_size.width + 4, text_size.height + 4));
		cv::rectangle(annotated_image, r, cv::Scalar(255,255,255), CV_FILLED);
		cv::putText(annotated_image, str, cv::Point(2, text_size.height + 3), font_face, annotation_font_scale, cv::Scalar(0,0,0), annotation_font_thickness, CV_AA);
	}

	return annotated_image;
}


image DarkHelp::convert_opencv_mat_to_darknet_image(cv::Mat mat)
{
	// this function is taken/inspired directly from Darknet:  image_opencv.cpp, mat_to_image()

	// OpenCV uses BGR, but Darknet expects RGB
	if (mat.channels() == 3)
	{
		cv::Mat rgb;
		cv::cvtColor(mat, rgb, cv::COLOR_BGR2RGB);
		mat = rgb;
	}

	const int width		= mat.cols;
	const int height	= mat.rows;
	const int channels	= mat.channels();
	const int step		= mat.step;
	image img			= make_image(width, height, channels);
	uint8_t * data		= (uint8_t*)mat.data;

	for (int y = 0; y < height; ++y)
	{
		for (int c = 0; c < channels; ++c)
		{
			for (int x = 0; x < width; ++x)
			{
				img.data[c*width*height + y*width + x] = data[y*step + x*channels + c] / 255.0f;
			}
		}
	}

	return img;
}


cv::Mat DarkHelp::convert_darknet_image_to_opencv_mat(const image img)
{
	// this function is taken/inspired directly from Darknet:  image_opencv.cpp, image_to_mat()

	const int channels	= img.c;
	const int width		= img.w;
	const int height	= img.h;
	cv::Mat mat			= cv::Mat(height, width, CV_8UC(channels));
	const int step		= mat.step;

	for (int y = 0; y < height; ++y)
	{
		for (int x = 0; x < width; ++x)
		{
			for (int c = 0; c < channels; ++c)
			{
				float val = img.data[c*height*width + y*width + x];
				mat.data[y*step + x*channels + c] = (unsigned char)(val * 255);
			}
		}
	}

	// But now the mat is in RGB instead of the BGR format that OpenCV expects to use.  See show_image_cv()
	// in Darknet which does the RGB<->BGR conversion, which we'll copy here so the mat is immediately usable.
	if (channels == 3)
	{
		cv::cvtColor(mat, mat, cv::COLOR_RGB2BGR);
	}

	return mat;
}


std::string DarkHelp::duration_string()
{
	std::string str;
	if		(duration <= std::chrono::nanoseconds(1000))	{ str = std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>	(duration).count()) + " nanoseconds";	}
	else if	(duration <= std::chrono::microseconds(1000))	{ str = std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(duration).count()) + " microseconds";	}
	else if	(duration <= std::chrono::milliseconds(1000))	{ str = std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(duration).count()) + " milliseconds";	}
	else /* use milliseconds for anything longer */			{ str = std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(duration).count()) + " milliseconds";	}

	return str;
}


DarkHelp::PredictionResults DarkHelp::predict(const float new_threshold)
{
	prediction_results.clear();
	annotated_image = cv::Mat();

	if (net == nullptr)
	{
		/// @throw std::logic_error if the network is invalid.
		throw std::logic_error("cannot predict with an empty network");
	}

	if (original_image.empty())
	{
		/// @throw std::logic_error if the image is invalid.
		throw std::logic_error("cannot predict with an empty image");
	}

	if (new_threshold >= 0.0)
	{
		threshold = new_threshold;
	}

	cv::Mat resized_image;
	cv::resize(original_image, resized_image, cv::Size(net->w, net->h));
	image img = convert_opencv_mat_to_darknet_image(resized_image);

	float * X = img.data;

	const auto t1 = std::chrono::high_resolution_clock::now();
	network_predict(*net, X);
	const auto t2 = std::chrono::high_resolution_clock::now();
	duration = t2 - t1;

	int nboxes = 0;
	const int use_letterbox = 0;
	auto darknet_results = get_network_boxes(net, original_image.cols, original_image.rows, threshold, hierchy_threshold, 0, 1, &nboxes, use_letterbox);

	if (non_maximal_suppression_threshold)
	{
		auto nw_layer = net->layers[net->n - 1];
		do_nms_sort(darknet_results, nboxes, nw_layer.classes, non_maximal_suppression_threshold);
	}

	for (int detection_idx = 0; detection_idx < nboxes; detection_idx ++)
	{
		const auto & det = darknet_results[detection_idx];

		for (int class_idx = 0; class_idx < det.classes; class_idx ++)
		{
			if (det.prob[class_idx] >= threshold)
			{
				const int w = std::round(det.bbox.w * original_image.cols);
				const int h = std::round(det.bbox.h * original_image.rows);
				const int x = std::round(det.bbox.x * original_image.cols - w/2.0);
				const int y = std::round(det.bbox.y * original_image.rows - h/2.0);

#if 0
				std::cout	<< "detection index=" << detection_idx
							<< " probability=" << (100.0 * det.prob[class_idx]) << "%"
							<< " class index=" << class_idx
							<< " box (x=" << det.bbox.x << " y=" << det.bbox.y << " w=" << det.bbox.w << " h=" << det.bbox.h << ")"
							<< " classes=" << det.classes
							<< " prob=" << (det.prob == nullptr ? "unknown" : std::to_string(*det.prob))
							<< " mask=" << (det.mask == nullptr ? "unknown" : std::to_string(*det.mask))
							<< " objectness=" << det.objectness
							<< " sort_class=" << det.sort_class
							<< " image (x=" << x
							<< " y=" << y
							<< " w=" << w
							<< " h=" << h << ")"
							<< std::endl;
#endif
				PredictionResult pr;
				pr.rect			= cv::Rect(cv::Point(x, y), cv::Size(w, h));
				pr.class_id		= det.sort_class;
				pr.probability	= det.prob[class_idx];
				if (static_cast<int>(names.size()) >= pr.class_id)
				{
					pr.name = names.at(pr.class_id);
				}
				else
				{
					pr.name = "#" + std::to_string(pr.class_id);
				}
				prediction_results.push_back(pr);
			}
		}
	}

	free_detections(darknet_results, nboxes);
	free_image(img);

	return prediction_results;
}


int main()
{
	DarkHelp dark_help("stone_barcodes_yolov3-tiny.cfg", "stone_barcodes_yolov3-tiny_final.weights", "stone_barcodes.names");

	for (int counter = 0; counter < 10; counter ++)
	{
		const std::string filename = "barcode_" + std::to_string(counter) + ".jpg";
		cv::Mat mat = cv::imread(filename);
		cv::Mat resized;
		cv::resize(mat, resized, cv::Size(640, 640));

		dark_help.predict(resized);
		dark_help.annotate();


		cv::imshow("test", dark_help.annotated_image);
		cv::waitKey();
	}

	return 0;
}
