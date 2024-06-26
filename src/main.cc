/*****************************************************************************
 * | Author      :   Luckfox team
 * | Function    :
 * | Info        :
 *
 *----------------
 * | This version:   V1.0
 * | Date        :   2024-04-07
 * | Info        :   Basic version
 *
 ******************************************************************************/

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/poll.h>
#include <time.h>
#include <unistd.h>
#include <vector>

#include "rtsp_demo.h"
#include "luckfox_mpi.h"

#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <rknn_api.h>

#define MODEL_WIDTH 28
#define MODEL_HEIGHT 28
#define CHANNEL_NUM 1

rknn_tensor_type input_type = RKNN_TENSOR_UINT8;
rknn_tensor_format input_layout = RKNN_TENSOR_NHWC;

rknn_context ctx = 0;

// 量化模型的npu输出结果为int8数据类型，后处理要按照int8数据类型处理
// 如下提供了int8排布的NC1HWC2转换成int8的nchw转换代码
int NC1HWC2_int8_to_NCHW_int8(const int8_t *src, int8_t *dst, int *dims, int channel, int h, int w)
{
	int batch = dims[0];
	int C1 = dims[1];
	int C2 = dims[4];
	int hw_src = dims[2] * dims[3];
	int hw_dst = h * w;
	for (int i = 0; i < batch; i++)
	{
		src = src + i * C1 * hw_src * C2;
		dst = dst + i * channel * hw_dst;
		for (int c = 0; c < channel; ++c)
		{
			int plane = c / C2;
			const int8_t *src_c = plane * hw_src * C2 + src;
			int offset = c % C2;
			for (int cur_h = 0; cur_h < h; ++cur_h)
				for (int cur_w = 0; cur_w < w; ++cur_w)
				{
					int cur_hw = cur_h * w + cur_w;
					dst[c * hw_dst + cur_h * w + cur_w] = src_c[C2 * cur_hw + offset];
				}
		}
	}

	return 0;
}

static unsigned char *load_model(const char *filename, int *model_size)
{
	FILE *fp = fopen(filename, "rb");
	if (fp == nullptr)
	{
		printf("fopen %s fail!\n", filename);
		return NULL;
	}
	fseek(fp, 0, SEEK_END);
	int model_len = ftell(fp);
	unsigned char *model = (unsigned char *)malloc(model_len); // 申请模型大小的内存，返回指针
	fseek(fp, 0, SEEK_SET);
	if (model_len != fread(model, 1, model_len, fp))
	{
		printf("fread %s fail!\n", filename);
		free(model);
		return NULL;
	}
	*model_size = model_len;
	if (fp)
	{
		fclose(fp);
	}
	return model;
}

static void dump_tensor_attr(rknn_tensor_attr *attr)
{
	char dims[128] = {0};
	for (int i = 0; i < attr->n_dims; ++i)
	{
		int idx = strlen(dims);
		sprintf(&dims[idx], "%d%s", attr->dims[i], (i == attr->n_dims - 1) ? "" : ", ");
	}
	// printf("  index=%d, name=%s, n_dims=%d, dims=[%s], n_elems=%d, size=%d, fmt=%s, type=%s, qnt_type=%s, "
	// 	   "zp=%d, scale=%f\n",
	// 	   attr->index, attr->name, attr->n_dims, dims, attr->n_elems, attr->size, get_format_string(attr->fmt),
	// 	   get_type_string(attr->type), get_qnt_type_string(attr->qnt_type), attr->zp, attr->scale);
}

// 定义一个函数用于从NCHW格式数据中获取预测的数字
int get_predicted_digit(int8_t *output_mems_nchw)
{ // 获取第一个输出的数据指针
	int8_t *output_data = output_mems_nchw;

	// 打印输出数据的值
	printf("Output data values:\n");
	for (int i = 0; i < 10; ++i)
	{
		printf("%d ", static_cast<int>(output_data[i]));
	}
	printf("\n");

	// 计算最大值的索引
	int predicted_digit = std::distance(output_data, std::max_element(output_data, output_data + 10));

	// 打印预测的数字
	printf("Predicted digit: %d\n", predicted_digit);

	return predicted_digit;
}

cv::Rect find_digit_contour(const cv::Mat &image)
{
	cv::Mat gray, blurred, edged;
	cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);
	cv::GaussianBlur(gray, blurred, cv::Size(5, 5), 0);
	cv::Canny(blurred, edged, 30, 150);

	// 应用形态学操作
	cv::Mat kernel = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(5, 5));
	cv::dilate(edged, edged, kernel);
	cv::erode(edged, edged, kernel);

	std::vector<std::vector<cv::Point>> contours;
	cv::findContours(edged, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

	if (contours.empty())
	{
		return cv::Rect();
	}

	// 找到最大的轮廓
	auto largest_contour = std::max_element(contours.begin(), contours.end(),
											[](const std::vector<cv::Point> &a, const std::vector<cv::Point> &b)
											{
												return cv::contourArea(a) < cv::contourArea(b);
											});

	return cv::boundingRect(*largest_contour);
}
cv::Mat preprocess_digit_region(const cv::Mat &region)
{
	cv::Mat gray, resized, normalized;
	cv::cvtColor(region, gray, cv::COLOR_BGR2GRAY);
	cv::resize(gray, resized, cv::Size(28, 28), 0, 0, cv::INTER_LINEAR);
	// resized.convertTo(normalized, CV_32F, 1.0 / 255.0);
	resized.convertTo(normalized, CV_8U, 255.0);
	return normalized;
}

int run_inference(cv::Mat &frame)
{
	int ret = 0;
	rknn_input_output_num io_num;

	// 获取输入输出的通道树
	rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));

	rknn_tensor_attr input_attrs[io_num.n_input];
	memset(input_attrs, 0, io_num.n_input * sizeof(rknn_tensor_attr));
	for (uint32_t i = 0; i < io_num.n_input; i++)
	{
		input_attrs[i].index = i;
		// query info
		ret = rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &(input_attrs[i]), sizeof(rknn_tensor_attr));
		if (ret < 0)
		{
			printf("rknn_init error! ret=%d\n", ret);
			return -1;
		}
		dump_tensor_attr(&input_attrs[i]);
	}

	printf("output tensors:\n");
	rknn_tensor_attr output_attrs[io_num.n_output];
	memset(output_attrs, 0, io_num.n_output * sizeof(rknn_tensor_attr));
	for (uint32_t i = 0; i < io_num.n_output; i++)
	{
		output_attrs[i].index = i;
		// query info
		ret = rknn_query(ctx, RKNN_QUERY_NATIVE_OUTPUT_ATTR, &(output_attrs[i]), sizeof(rknn_tensor_attr));
		if (ret != RKNN_SUCC)
		{
			printf("rknn_query fail! ret=%d\n", ret);
			return -1;
		}
		dump_tensor_attr(&output_attrs[i]);
	}

	// cv::Mat gray;
	// cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
	// cv::resize(gray, gray, cv::Size(28, 28), 0, 0, cv::INTER_LINEAR);

	printf("Gray image size: %dx%d\n", frame.rows, frame.cols);
	printf("Gray image type: %d\n", frame.type());
	// 为resize后的图形申请内存
	int mem_size = MODEL_WIDTH * MODEL_HEIGHT * CHANNEL_NUM;
	unsigned char *resize_buf = (unsigned char *)malloc(mem_size);
	memset(resize_buf, 0, mem_size);

	// Create input tensor memory
	rknn_tensor_mem *input_mems[1];
	// default input type is int8 (normalize and quantize need compute in outside)
	// if set uint8, will fuse normalize and quantize to npu
	input_attrs[0].type = input_type;
	// default fmt is NHWC, npu only support NHWC in zero copy mode
	input_attrs[0].fmt = input_layout;

	input_mems[0] = rknn_create_mem(ctx, input_attrs[0].size_with_stride);

	// Copy input data to input tensor memory
	int width = input_attrs[0].dims[2];
	int stride = input_attrs[0].w_stride;

	if (width == stride)
	{
		memcpy(input_mems[0]->virt_addr, frame.data, width * input_attrs[0].dims[1] * input_attrs[0].dims[3]);
	}
	else
	{
		int height = input_attrs[0].dims[1];
		int channel = input_attrs[0].dims[3];
		// copy from src to dst with stride
		uint8_t *src_ptr = frame.data;
		uint8_t *dst_ptr = (uint8_t *)input_mems[0]->virt_addr;
		// width-channel elements
		int src_wc_elems = width * channel;
		int dst_wc_elems = stride * channel;
		for (int h = 0; h < height; ++h)
		{
			memcpy(dst_ptr, src_ptr, src_wc_elems);
			src_ptr += src_wc_elems;
			dst_ptr += dst_wc_elems;
		}
	}

	// Create output tensor memory
	rknn_tensor_mem *output_mems[io_num.n_output];
	for (uint32_t i = 0; i < io_num.n_output; ++i)
	{
		output_mems[i] = rknn_create_mem(ctx, output_attrs[i].size_with_stride);
	}

	// Set input tensor memory
	ret = rknn_set_io_mem(ctx, input_mems[0], &input_attrs[0]);
	if (ret < 0)
	{
		printf("rknn_set_io_mem fail! ret=%d\n", ret);
		return -1;
	}

	// Set output tensor memory
	for (uint32_t i = 0; i < io_num.n_output; ++i)
	{
		// set output memory and attribute
		ret = rknn_set_io_mem(ctx, output_mems[i], &output_attrs[i]);
		if (ret < 0)
		{
			printf("rknn_set_io_mem fail! ret=%d\n", ret);
			return -1;
		}
	}

	// 运行推理
	ret = rknn_run(ctx, nullptr);
	if (ret < 0)
	{
		printf("rknn_run failed! %s\n", ret);
		return -1;
	}

	printf("output origin tensors:\n");
	rknn_tensor_attr orig_output_attrs[io_num.n_output];
	memset(orig_output_attrs, 0, io_num.n_output * sizeof(rknn_tensor_attr));
	for (uint32_t i = 0; i < io_num.n_output; i++)
	{
		orig_output_attrs[i].index = i;
		// query info
		ret = rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &(orig_output_attrs[i]), sizeof(rknn_tensor_attr));
		if (ret != RKNN_SUCC)
		{
			printf("rknn_query fail! ret=%d\n", ret);
			return -1;
		}
		dump_tensor_attr(&orig_output_attrs[i]);
	}

	// 创建存储模型输出的NCHW格式数据的向量
	int8_t *output_mems_nchw[io_num.n_output];
	for (uint32_t i = 0; i < io_num.n_output; ++i)
	{
		int size = orig_output_attrs[i].size_with_stride;
		output_mems_nchw[i] = (int8_t *)malloc(size);
	}

	// 进行NC1HWC2_int8_to_NCHW_int8转换
	// for (uint32_t i = 0; i < io_num.n_output; i++)
	// {
	// 	int channel = orig_output_attrs[i].dims[1];
	// 	int h = orig_output_attrs[i].n_dims > 2 ? orig_output_attrs[i].dims[2] : 1;
	// 	int w = orig_output_attrs[i].n_dims > 3 ? orig_output_attrs[i].dims[3] : 1;
	// 	int hw = h * w;
	// 	NC1HWC2_int8_to_NCHW_int8((int8_t *)output_mems[i]->virt_addr, (int8_t *)output_mems_nchw[i],
	// 							  (int *)output_attrs[i].dims, channel, h, w);
	// }

	// 获取预测的数字
	int predicted_digit = get_predicted_digit((int8_t *)output_mems_nchw[0]);

	// 释放内存

	rknn_destroy_mem(ctx, input_mems[0]);
	for (uint32_t i = 0; i < io_num.n_output; ++i)
	{
		rknn_destroy_mem(ctx, output_mems[i]);
		free(output_mems_nchw[i]);
	}

	return predicted_digit;
}

int main(int argc, char *argv[])
{
	RK_S32 s32Ret = 0;

	int sX, sY, eX, eY;
	int width = 640;
	int height = 480;

	// rknn init
	int ret;
	int model_len = 0;
	if (argc != 3)
	{
		printf("Usage: %s <model.rknn> <image_path>\n", argv[0]);
		return -1;
	}

	char *model_path = argv[1];
	const char *image_path = argv[2];

	unsigned char *model = load_model(model_path, &model_len);

	ret = rknn_init(&ctx, model, model_len, 0, NULL);
	if (ret < 0)
	{
		printf("rknn_init failed! ret=%d", ret);
		return -1;
	}
	// Get sdk and driver version
	rknn_sdk_version sdk_ver;
	ret = rknn_query(ctx, RKNN_QUERY_SDK_VERSION, &sdk_ver, sizeof(sdk_ver));
	if (ret != RKNN_SUCC)
	{
		printf("rknn_query fail! ret=%d\n", ret);
		return -1;
	}
	printf("rknn_api/rknnrt version: %s, driver version: %s\n", sdk_ver.api_version, sdk_ver.drv_version);

	// Get Model Input Output Info
	rknn_input_output_num io_num;
	ret = rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
	if (ret != RKNN_SUCC)
	{
		printf("rknn_query fail! ret=%d\n", ret);
		return -1;
	}
	printf("model input num: %d, output num: %d\n", io_num.n_input, io_num.n_output);

	cv::Mat img = cv::imread(image_path);
	if (img.empty())
	{
		printf("Open image %s failed!\n", image_path);
		return -1;
	}

	cv::Mat preprocessed_img = preprocess_digit_region(img);
	int prediction = run_inference(img);

	printf("Predicted digit: %d\n", prediction);

	// RKNN
	rknn_destroy(ctx);
	free(model);

	return 0;
}
