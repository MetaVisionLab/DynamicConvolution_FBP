#ifdef USE_OPENCV
#include <opencv2/core/core.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#endif  // USE_OPENCV

#include <string>
#include <vector>

#include "caffe/data_transformer.hpp"
#include "caffe/util/io.hpp"
#include "caffe/util/math_functions.hpp"
#include "caffe/util/rng.hpp"
#include <time.h>
#include <stdlib.h>

namespace caffe {

template<typename Dtype>
DataTransformer<Dtype>::DataTransformer(const TransformationParameter& param,
    Phase phase)
    : param_(param), phase_(phase) {
  // check if we want to use mean_file
  if (param_.has_mean_file()) {
    CHECK_EQ(param_.mean_value_size(), 0) <<
      "Cannot specify mean_file and mean_value at the same time";
    const string& mean_file = param.mean_file();
    if (Caffe::root_solver()) {
      LOG(INFO) << "Loading mean file from: " << mean_file;
    }
    BlobProto blob_proto;
    ReadProtoFromBinaryFileOrDie(mean_file.c_str(), &blob_proto);
    data_mean_.FromProto(blob_proto);
  }
  // check if we want to use mean_value
  if (param_.mean_value_size() > 0) {
    CHECK(param_.has_mean_file() == false) <<
      "Cannot specify mean_file and mean_value at the same time";
    for (int c = 0; c < param_.mean_value_size(); ++c) {
      mean_values_.push_back(param_.mean_value(c));
    }
  }
}

template<typename Dtype>
void DataTransformer<Dtype>::Transform(const Datum& datum,
                                       Dtype* transformed_data) {
  const string& data = datum.data();
  const int datum_channels = datum.channels();
  const int datum_height = datum.height();
  const int datum_width = datum.width();

  const int crop_size = param_.crop_size();
  const Dtype scale = param_.scale();
  const bool do_mirror = param_.mirror() && Rand(2);
  const bool has_mean_file = param_.has_mean_file();
  const bool has_uint8 = data.size() > 0;
  const bool has_mean_values = mean_values_.size() > 0;

  CHECK_GT(datum_channels, 0);
  CHECK_GE(datum_height, crop_size);
  CHECK_GE(datum_width, crop_size);

  Dtype* mean = NULL;
  if (has_mean_file) {
    CHECK_EQ(datum_channels, data_mean_.channels());
    CHECK_EQ(datum_height, data_mean_.height());
    CHECK_EQ(datum_width, data_mean_.width());
    mean = data_mean_.mutable_cpu_data();
  }
  if (has_mean_values) {
    CHECK(mean_values_.size() == 1 || mean_values_.size() == datum_channels) <<
     "Specify either 1 mean_value or as many as channels: " << datum_channels;
    if (datum_channels > 1 && mean_values_.size() == 1) {
      // Replicate the mean_value for simplicity
      for (int c = 1; c < datum_channels; ++c) {
        mean_values_.push_back(mean_values_[0]);
      }
    }
  }

  int height = datum_height;
  int width = datum_width;

  int h_off = 0;
  int w_off = 0;
  if (crop_size) {
    height = crop_size;
    width = crop_size;
    // We only do random crop when we do training.
    if (phase_ == TRAIN) {
      h_off = Rand(datum_height - crop_size + 1);
      w_off = Rand(datum_width - crop_size + 1);
    } else {
      h_off = (datum_height - crop_size) / 2;
      w_off = (datum_width - crop_size) / 2;
    }
  }

//// linluojun
  Dtype datum_element;
  int tmp_index, data_index, top_index;

  if (param_.random_crop() && phase_ == TRAIN) {
    if (param_.crop_area_size() > 0) {
      for (int c = 0; c < param_.crop_area_size(); ++c) {
        crop_area_.push_back(param_.crop_area(c));
        aspect_ratio_.push_back(param_.aspect_ratio(c));
      }
    }
    CHECK_GT(crop_area_[1], crop_area_[0]);
    CHECK_GT(aspect_ratio_[1], aspect_ratio_[0]);
    float area = Randfloat(crop_area_[0], crop_area_[1]) * datum_height * datum_width;
    float ratio = Randfloat(aspect_ratio_[0], aspect_ratio_[1]);
    int crop_h, crop_w;
    crop_h = sqrt(area * ratio);
    crop_w = sqrt(area / ratio);
    if (phase_ == TRAIN) {
      h_off = Rand(datum_height - crop_h + 1);
      w_off = Rand(datum_width - crop_w + 1);
    } 
    // transform data
    Dtype* tmp = new Dtype(datum_channels * crop_h * crop_w);
    for (int c = 0; c < datum_channels; ++c) {
      for (int h = 0; h < crop_h; ++h) {
        for (int w = 0; w < crop_w; ++w) {
          data_index = (c * datum_height + h_off + h) * datum_width + w_off + w;
          if (do_mirror) {
            tmp_index = (c * crop_h + h) * crop_w + (crop_w - 1 - w);
          } else {
            tmp_index = (c * crop_h + h) * crop_w + w;
          }
          if (has_uint8) {
            datum_element = static_cast<Dtype>(static_cast<uint8_t>(data[data_index]));
          } else {
            datum_element = datum.float_data(data_index);
          }
          if (has_mean_file) {
            tmp[tmp_index] = (datum_element - mean[data_index]) * scale;
          } else {
            if (has_mean_values) {
              tmp[tmp_index] = (datum_element - mean_values_[c]) * scale;
            } else {
              tmp[tmp_index] = datum_element * scale;
            }
          }
        }
      }
    } 
    // billinear interpolation
    const float rheight = static_cast<float>(crop_h - 1) / (height - 1);
    const float rwidth = static_cast<float>(crop_w - 1) / (width - 1);
    for (int h2 = 0; h2 < height; ++h2) {
      const float h1r = rheight * h2;
      const int h1 = h1r;
      const int h1p = (h1 < crop_h - 1) ? 1 : 0; // chao guo bian jie??
      const Dtype h1lambda = h1r - h1;
      const Dtype h0lambda = Dtype(1.) - h1lambda;
      for (int w2 = 0; w2 < width; ++w2) {
        const float w1r = rwidth * w2;
        const int w1 = w1r;
        const int w1p = (w1 < crop_w - 1) ? 1 : 0;
        const Dtype w1lambda = w1r - w1;
        const Dtype w0lambda = Dtype(1.) - w1lambda;
        tmp_index = h1 * crop_h + w1;
        top_index = h2 * height + w2;
        Dtype* pos1 = &tmp[tmp_index];
        Dtype* pos2 = &transformed_data[top_index];
        for (int c = 0; c < datum_channels; ++c) {
          pos2[0] =
            h0lambda * (w0lambda * pos1[0]            + w1lambda * pos1[w1p]) + 
            h1lambda * (w0lambda * pos1[h1p * crop_w] + w1lambda * pos1[h1p * crop_w + w1p]);
          pos1 += crop_w * crop_h;
          pos2 += height * width;
        }
      }
    }
  delete tmp;
  } else { 
    for (int c = 0; c < datum_channels; ++c) {
      for (int h = 0; h < height; ++h) {
        for (int w = 0; w < width; ++w) {
          data_index = (c * datum_height + h_off + h) * datum_width + w_off + w;
          if (do_mirror) {
            top_index = (c * height + h) * width + (width - 1 - w);
          } else {
            top_index = (c * height + h) * width + w;
          }
          if (has_uint8) {
            datum_element =
              static_cast<Dtype>(static_cast<uint8_t>(data[data_index]));
          } else {
            datum_element = datum.float_data(data_index);
          }
          if (has_mean_file) {
            transformed_data[top_index] =
              (datum_element - mean[data_index]) * scale;
          } else {
            if (has_mean_values) {
              transformed_data[top_index] =
                (datum_element - mean_values_[c]) * scale;
            } else {
              transformed_data[top_index] = datum_element * scale;
            }
          }
        }
      }
    }  
  } 
}


template<typename Dtype>
void DataTransformer<Dtype>::Transform(const Datum& datum,
                                       Blob<Dtype>* transformed_blob) {
  // If datum is encoded, decode and transform the cv::image.
  if (datum.encoded()) {
#ifdef USE_OPENCV
    CHECK(!(param_.force_color() && param_.force_gray()))
        << "cannot set both force_color and force_gray";
    cv::Mat cv_img;
    if (param_.force_color() || param_.force_gray()) {
    // If force_color then decode in color otherwise decode in gray.
      cv_img = DecodeDatumToCVMat(datum, param_.force_color());
    } else {
      cv_img = DecodeDatumToCVMatNative(datum);
    }
    // Transform the cv::image into blob.
    return Transform(cv_img, transformed_blob);
#else
    LOG(FATAL) << "Encoded datum requires OpenCV; compile with USE_OPENCV.";
#endif  // USE_OPENCV
  } else {
    if (param_.force_color() || param_.force_gray()) {
      LOG(ERROR) << "force_color and force_gray only for encoded datum";
    }
  }

  const int crop_size = param_.crop_size();
  const int datum_channels = datum.channels();
  const int datum_height = datum.height();
  const int datum_width = datum.width();

  // Check dimensions.
  const int channels = transformed_blob->channels();
  const int height = transformed_blob->height();
  const int width = transformed_blob->width();
  const int num = transformed_blob->num();

  CHECK_EQ(channels, datum_channels);
  CHECK_LE(height, datum_height);
  CHECK_LE(width, datum_width);
  CHECK_GE(num, 1);

  if (crop_size) {
    CHECK_EQ(crop_size, height);
    CHECK_EQ(crop_size, width);
  } else {
    CHECK_EQ(datum_height, height);
    CHECK_EQ(datum_width, width);
  }

  Dtype* transformed_data = transformed_blob->mutable_cpu_data();
  Transform(datum, transformed_data);
}

template<typename Dtype>
void DataTransformer<Dtype>::Transform(const vector<Datum> & datum_vector,
                                       Blob<Dtype>* transformed_blob) {
  const int datum_num = datum_vector.size();
  const int num = transformed_blob->num();
  const int channels = transformed_blob->channels();
  const int height = transformed_blob->height();
  const int width = transformed_blob->width();

  CHECK_GT(datum_num, 0) << "There is no datum to add";
  CHECK_LE(datum_num, num) <<
    "The size of datum_vector must be no greater than transformed_blob->num()";
  Blob<Dtype> uni_blob(1, channels, height, width);
  for (int item_id = 0; item_id < datum_num; ++item_id) {
    int offset = transformed_blob->offset(item_id);
    uni_blob.set_cpu_data(transformed_blob->mutable_cpu_data() + offset);
    Transform(datum_vector[item_id], &uni_blob);
  }
}

#ifdef USE_OPENCV
template<typename Dtype>
void DataTransformer<Dtype>::Transform(const vector<cv::Mat> & mat_vector,
                                       Blob<Dtype>* transformed_blob) {
  const int mat_num = mat_vector.size();
  const int num = transformed_blob->num();
  const int channels = transformed_blob->channels();
  const int height = transformed_blob->height();
  const int width = transformed_blob->width();

  CHECK_GT(mat_num, 0) << "There is no MAT to add";
  CHECK_EQ(mat_num, num) <<
    "The size of mat_vector must be equals to transformed_blob->num()";
  Blob<Dtype> uni_blob(1, channels, height, width);
  for (int item_id = 0; item_id < mat_num; ++item_id) {
    int offset = transformed_blob->offset(item_id);
    uni_blob.set_cpu_data(transformed_blob->mutable_cpu_data() + offset);
    Transform(mat_vector[item_id], &uni_blob);
  }
}

template<typename Dtype>
void DataTransformer<Dtype>::Transform(const cv::Mat& cv_img,
                                       Blob<Dtype>* transformed_blob) {
  const int crop_size = param_.crop_size();
  const int img_channels = cv_img.channels();
  const int img_height = cv_img.rows;
  const int img_width = cv_img.cols;

  // Check dimensions.
  const int channels = transformed_blob->channels();
  const int height = transformed_blob->height();
  const int width = transformed_blob->width();
  const int num = transformed_blob->num();

  CHECK_EQ(channels, img_channels);
  CHECK_LE(height, img_height);
  CHECK_LE(width, img_width);
  CHECK_GE(num, 1);

  CHECK(cv_img.depth() == CV_8U) << "Image data type must be unsigned byte";

  const Dtype scale = param_.scale();
  const bool do_mirror = param_.mirror() && Rand(2);
  const bool has_mean_file = param_.has_mean_file();
  const bool has_mean_values = mean_values_.size() > 0;

  CHECK_GT(img_channels, 0);
  CHECK_GE(img_height, crop_size);
  CHECK_GE(img_width, crop_size);

  Dtype* mean = NULL;
  if (has_mean_file) {
    CHECK_EQ(img_channels, data_mean_.channels());
    CHECK_EQ(img_height, data_mean_.height());
    CHECK_EQ(img_width, data_mean_.width());
    mean = data_mean_.mutable_cpu_data();
  }
  if (has_mean_values) {
    CHECK(mean_values_.size() == 1 || mean_values_.size() == img_channels) <<
     "Specify either 1 mean_value or as many as channels: " << img_channels;
    if (img_channels > 1 && mean_values_.size() == 1) {
      // Replicate the mean_value for simplicity
      for (int c = 1; c < img_channels; ++c) {
        mean_values_.push_back(mean_values_[0]);
      }
    }
  }

  int h_off = 0;
  int w_off = 0;
  cv::Mat cv_cropped_img = cv_img;
  if (crop_size) {
    CHECK_EQ(crop_size, height);
    CHECK_EQ(crop_size, width);
    // We only do random crop when we do training.
    if (phase_ == TRAIN) {
      h_off = Rand(img_height - crop_size + 1);
      w_off = Rand(img_width - crop_size + 1);
    } else {
      h_off = (img_height - crop_size) / 2;
      w_off = (img_width - crop_size) / 2;
    }
    cv::Rect roi(w_off, h_off, crop_size, crop_size);
    cv_cropped_img = cv_img(roi);
  } else {
    CHECK_EQ(img_height, height);
    CHECK_EQ(img_width, width);
  }

  CHECK(cv_cropped_img.data);
  Dtype* transformed_data = transformed_blob->mutable_cpu_data();
  int top_index;

////linluojun
  if (param_.random_crop() && phase_ == TRAIN) {
    if (param_.crop_area_size() > 0) {
      for (int c = 0; c < param_.crop_area_size(); ++c) {
        crop_area_.push_back(param_.crop_area(c));
        aspect_ratio_.push_back(param_.aspect_ratio(c));
      }
    }
    CHECK_GT(crop_area_[1], crop_area_[0]);
    CHECK_GT(aspect_ratio_[1], aspect_ratio_[0]);
    float area = Randfloat(crop_area_[0], crop_area_[1]) * img_height * img_width;
    float ratio = Randfloat(aspect_ratio_[0], aspect_ratio_[1]);
    int crop_h, crop_w;
    crop_h = sqrt(area * ratio);
    crop_h = crop_h > img_height ? img_height : crop_h;
    crop_w = sqrt(area / ratio);
    crop_w = crop_w > img_width ? img_width : crop_w;
    // LOG(INFO) << area / img_height / img_width << " " << ratio << " " << crop_h << " " << crop_w << " " << img_height << " " << img_width;
    h_off = Rand(img_height - crop_h + 1);
    w_off = Rand(img_width - crop_w + 1);
    // LOG(INFO) << w_off << " " << h_off << " " << crop_h << " " << crop_w;
    cv::Rect roi(w_off, h_off, crop_w, crop_h);
    cv::Mat tmp = cv_img;
    tmp = cv_img(roi);
    cv::resize(tmp, cv_cropped_img, cv::Size(crop_size, crop_size));
  }

  for (int h = 0; h < height; ++h) {
    const uchar* ptr = cv_cropped_img.ptr<uchar>(h);
    int img_index = 0;
    for (int w = 0; w < width; ++w) {
      for (int c = 0; c < img_channels; ++c) {
        if (do_mirror) {
          top_index = (c * height + h) * width + (width - 1 - w);
        } else {
          top_index = (c * height + h) * width + w;
        }
        // int top_index = (c * height + h) * width + w;
        Dtype pixel = static_cast<Dtype>(ptr[img_index++]);
        if (has_mean_file) {
          int mean_index = (c * img_height + h_off + h) * img_width + w_off + w;
          transformed_data[top_index] =
            (pixel - mean[mean_index]) * scale;
        } else {
          if (has_mean_values) {
            transformed_data[top_index] =
              (pixel - mean_values_[c]) * scale;
          } else {
            transformed_data[top_index] = pixel * scale;
          }
        }
      }
    }
  } 
}



// linluojun
template<typename Dtype>
int DataTransformer<Dtype>::Find_center(float* array, int len) {
  float count = 0;
  for(int k=0; k<len; k++) {
    count += array[k];
  }
  float seed = Randfloat(0.0, count);
  if(seed <= array[0]) {
    return 0;
  }
  for(int i=1; i<len; i++) {
    array[i] += array[i-1];
    if(seed <= array[i]) {
    // if(seed <= *(array + i)) {
      return i;
    }
  }
}

template<typename Dtype>
void DataTransformer<Dtype>::Attention_Transform(const cv::Mat& cv_img,
                                       Blob<Dtype>* transformed_blob,
                                       float* attention_array) {
  const int crop_size = param_.crop_size();
  const int img_channels = cv_img.channels();
  const int img_height = cv_img.rows;
  const int img_width = cv_img.cols;

  // Check dimensions.
  const int channels = transformed_blob->channels();
  const int height = transformed_blob->height();
  const int width = transformed_blob->width();
  const int num = transformed_blob->num();

  CHECK_EQ(channels, img_channels);
  CHECK_LE(height, img_height);
  CHECK_LE(width, img_width);
  CHECK_GE(num, 1);

  CHECK(cv_img.depth() == CV_8U) << "Image data type must be unsigned byte";

  const Dtype scale = param_.scale();
  const bool do_mirror = param_.mirror() && Rand(2);
  const bool has_mean_file = param_.has_mean_file();
  const bool has_mean_values = mean_values_.size() > 0;

  CHECK_GT(img_channels, 0);
  CHECK_GE(img_height, crop_size);
  CHECK_GE(img_width, crop_size);

  Dtype* mean = NULL;
  if (has_mean_file) {
    CHECK_EQ(img_channels, data_mean_.channels());
    CHECK_EQ(img_height, data_mean_.height());
    CHECK_EQ(img_width, data_mean_.width());
    mean = data_mean_.mutable_cpu_data();
  }
  if (has_mean_values) {
    CHECK(mean_values_.size() == 1 || mean_values_.size() == img_channels) <<
     "Specify either 1 mean_value or as many as channels: " << img_channels;
    if (img_channels > 1 && mean_values_.size() == 1) {
      // Replicate the mean_value for simplicity
      for (int c = 1; c < img_channels; ++c) {
        mean_values_.push_back(mean_values_[0]);
      }
    }
  }

  int h_off = 0;
  int w_off = 0;
  cv::Mat cv_cropped_img = cv_img;
  if (crop_size) {
    CHECK_EQ(crop_size, height);
    CHECK_EQ(crop_size, width);
  } else {
    CHECK_EQ(img_height, height);
    CHECK_EQ(img_width, width);
  }


  CHECK(cv_cropped_img.data);
  Dtype* transformed_data = transformed_blob->mutable_cpu_data();
  int top_index;

////linluojun
  float* attention_array_h = new float[img_height]();
  int idx = 0;
  for(int h = 0; h < img_height; ++h){
    attention_array_h[h] = 0;
    for(int w = 0; w < img_width; ++w, ++idx){
      attention_array_h[h] += attention_array[idx];
    }
  }
  int pos_h = Find_center(attention_array_h, img_height);
  int pos_w = Find_center(attention_array + pos_h * img_width, img_width);
  delete[] attention_array_h;


  if (param_.random_crop() && phase_ == TRAIN) {
    if (param_.crop_area_size() > 0) {
      for (int c = 0; c < param_.crop_area_size(); ++c) {
        crop_area_.push_back(param_.crop_area(c));
        aspect_ratio_.push_back(param_.aspect_ratio(c));
      }
    }
    CHECK_GT(crop_area_[1], crop_area_[0]);
    CHECK_GT(aspect_ratio_[1], aspect_ratio_[0]);
    float area = Randfloat(crop_area_[0], crop_area_[1]) * img_height * img_width;
    float ratio = Randfloat(aspect_ratio_[0], aspect_ratio_[1]);
    int crop_h, crop_w;
    crop_h = sqrt(area * ratio);
    crop_w = sqrt(area / ratio);
    // LOG(INFO) << area / img_height / img_width << " " << ratio << " " << crop_h << " " << crop_w << " " << img_height << " " << img_width;
    h_off = pos_h - crop_h / 2;
    h_off = h_off > 0 ? h_off: 0;
    w_off = pos_w - crop_w / 2;
    w_off = w_off > 0 ? w_off: 0;
    crop_h = (h_off + crop_h) > img_height ? (img_height - h_off) : crop_h;
    crop_w = (w_off + crop_w) > img_width ? (img_width - w_off) : crop_w;


    // LOG(INFO) << w_off << " " << h_off << " " << crop_h << " " << crop_w;
    cv::Rect roi(w_off, h_off, crop_w, crop_h);
    cv::Mat tmp = cv_img;
    tmp = cv_img(roi);
    cv::resize(tmp, cv_cropped_img, cv::Size(crop_size, crop_size));
  } else {  // phase = test
      h_off = (img_height - crop_size) / 2;
      w_off = (img_width - crop_size) / 2;
      cv::Rect roi(w_off, h_off, crop_size, crop_size);
      cv_cropped_img = cv_img(roi);
  }

  for (int h = 0; h < height; ++h) {
    const uchar* ptr = cv_cropped_img.ptr<uchar>(h);
    int img_index = 0;
    for (int w = 0; w < width; ++w) {
      for (int c = 0; c < img_channels; ++c) {
        if (do_mirror) {
          top_index = (c * height + h) * width + (width - 1 - w);
        } else {
          top_index = (c * height + h) * width + w;
        }
        // int top_index = (c * height + h) * width + w;
        Dtype pixel = static_cast<Dtype>(ptr[img_index++]);
        if (has_mean_file) {
          int mean_index = (c * img_height + h_off + h) * img_width + w_off + w;
          transformed_data[top_index] =
            (pixel - mean[mean_index]) * scale;
        } else {
          if (has_mean_values) {
            transformed_data[top_index] =
              (pixel - mean_values_[c]) * scale;
          } else {
            transformed_data[top_index] = pixel * scale;
          }
        }
      }
    }
  } 
}
#endif  // USE_OPENCV


template<typename Dtype>
void DataTransformer<Dtype>::Transform(Blob<Dtype>* input_blob,
                                       Blob<Dtype>* transformed_blob) {
  const int crop_size = param_.crop_size();
  const int input_num = input_blob->num();
  const int input_channels = input_blob->channels();
  const int input_height = input_blob->height();
  const int input_width = input_blob->width();

  if (transformed_blob->count() == 0) {
    // Initialize transformed_blob with the right shape.
    if (crop_size) {
      transformed_blob->Reshape(input_num, input_channels,
                                crop_size, crop_size);
    } else {
      transformed_blob->Reshape(input_num, input_channels,
                                input_height, input_width);
    }
  }

  const int num = transformed_blob->num();
  const int channels = transformed_blob->channels();
  const int height = transformed_blob->height();
  const int width = transformed_blob->width();
  const int size = transformed_blob->count();

  CHECK_LE(input_num, num);
  CHECK_EQ(input_channels, channels);
  CHECK_GE(input_height, height);
  CHECK_GE(input_width, width);


  const Dtype scale = param_.scale();
  const bool do_mirror = param_.mirror() && Rand(2);
  const bool has_mean_file = param_.has_mean_file();
  const bool has_mean_values = mean_values_.size() > 0;

  int h_off = 0;
  int w_off = 0;
  if (crop_size) {
    CHECK_EQ(crop_size, height);
    CHECK_EQ(crop_size, width);
    // We only do random crop when we do training.
    if (phase_ == TRAIN) {
      h_off = Rand(input_height - crop_size + 1);
      w_off = Rand(input_width - crop_size + 1);
    } else {
      h_off = (input_height - crop_size) / 2;
      w_off = (input_width - crop_size) / 2;
    }
  } else {
    CHECK_EQ(input_height, height);
    CHECK_EQ(input_width, width);
  }

  Dtype* input_data = input_blob->mutable_cpu_data();
  if (has_mean_file) {
    CHECK_EQ(input_channels, data_mean_.channels());
    CHECK_EQ(input_height, data_mean_.height());
    CHECK_EQ(input_width, data_mean_.width());
    for (int n = 0; n < input_num; ++n) {
      int offset = input_blob->offset(n);
      caffe_sub(data_mean_.count(), input_data + offset,
            data_mean_.cpu_data(), input_data + offset);
    }
  }

  if (has_mean_values) {
    CHECK(mean_values_.size() == 1 || mean_values_.size() == input_channels) <<
     "Specify either 1 mean_value or as many as channels: " << input_channels;
    if (mean_values_.size() == 1) {
      caffe_add_scalar(input_blob->count(), -(mean_values_[0]), input_data);
    } else {
      for (int n = 0; n < input_num; ++n) {
        for (int c = 0; c < input_channels; ++c) {
          int offset = input_blob->offset(n, c);
          caffe_add_scalar(input_height * input_width, -(mean_values_[c]),
            input_data + offset);
        }
      }
    }
  }

  Dtype* transformed_data = transformed_blob->mutable_cpu_data();

  for (int n = 0; n < input_num; ++n) {
    int top_index_n = n * channels;
    int data_index_n = n * channels;
    for (int c = 0; c < channels; ++c) {
      int top_index_c = (top_index_n + c) * height;
      int data_index_c = (data_index_n + c) * input_height + h_off;
      for (int h = 0; h < height; ++h) {
        int top_index_h = (top_index_c + h) * width;
        int data_index_h = (data_index_c + h) * input_width + w_off;
        if (do_mirror) {
          int top_index_w = top_index_h + width - 1;
          for (int w = 0; w < width; ++w) {
            transformed_data[top_index_w-w] = input_data[data_index_h + w];
          }
        } else {
          for (int w = 0; w < width; ++w) {
            transformed_data[top_index_h + w] = input_data[data_index_h + w];
          }
        }
      }
    }
  }
  if (scale != Dtype(1)) {
    DLOG(INFO) << "Scale: " << scale;
    caffe_scal(size, scale, transformed_data);
  }
}

template<typename Dtype>
vector<int> DataTransformer<Dtype>::InferBlobShape(const Datum& datum) {
  if (datum.encoded()) {
#ifdef USE_OPENCV
    CHECK(!(param_.force_color() && param_.force_gray()))
        << "cannot set both force_color and force_gray";
    cv::Mat cv_img;
    if (param_.force_color() || param_.force_gray()) {
    // If force_color then decode in color otherwise decode in gray.
      cv_img = DecodeDatumToCVMat(datum, param_.force_color());
    } else {
      cv_img = DecodeDatumToCVMatNative(datum);
    }
    // InferBlobShape using the cv::image.
    return InferBlobShape(cv_img);
#else
    LOG(FATAL) << "Encoded datum requires OpenCV; compile with USE_OPENCV.";
#endif  // USE_OPENCV
  }
  const int crop_size = param_.crop_size();
  const int datum_channels = datum.channels();
  const int datum_height = datum.height();
  const int datum_width = datum.width();
  // Check dimensions.
  CHECK_GT(datum_channels, 0);
  CHECK_GE(datum_height, crop_size);
  CHECK_GE(datum_width, crop_size);
  // Build BlobShape.
  vector<int> shape(4);
  shape[0] = 1;
  shape[1] = datum_channels;
  shape[2] = (crop_size)? crop_size: datum_height;
  shape[3] = (crop_size)? crop_size: datum_width;
  return shape;
}

template<typename Dtype>
vector<int> DataTransformer<Dtype>::InferBlobShape(
    const vector<Datum> & datum_vector) {
  const int num = datum_vector.size();
  CHECK_GT(num, 0) << "There is no datum to in the vector";
  // Use first datum in the vector to InferBlobShape.
  vector<int> shape = InferBlobShape(datum_vector[0]);
  // Adjust num to the size of the vector.
  shape[0] = num;
  return shape;
}

#ifdef USE_OPENCV
template<typename Dtype>
vector<int> DataTransformer<Dtype>::InferBlobShape(const cv::Mat& cv_img) {
  const int crop_size = param_.crop_size();
  const int img_channels = cv_img.channels();
  const int img_height = cv_img.rows;
  const int img_width = cv_img.cols;
  // Check dimensions.
  CHECK_GT(img_channels, 0);
  CHECK_GE(img_height, crop_size);
  CHECK_GE(img_width, crop_size);
  // Build BlobShape.
  vector<int> shape(4);
  shape[0] = 1;
  shape[1] = img_channels;
  shape[2] = (crop_size)? crop_size: img_height;
  shape[3] = (crop_size)? crop_size: img_width;
  return shape;
}

template<typename Dtype>
vector<int> DataTransformer<Dtype>::InferBlobShape(
    const vector<cv::Mat> & mat_vector) {
  const int num = mat_vector.size();
  CHECK_GT(num, 0) << "There is no cv_img to in the vector";
  // Use first cv_img in the vector to InferBlobShape.
  vector<int> shape = InferBlobShape(mat_vector[0]);
  // Adjust num to the size of the vector.
  shape[0] = num;
  return shape;
}
#endif  // USE_OPENCV

template <typename Dtype>
void DataTransformer<Dtype>::InitRand() {
  const bool needs_rand = param_.mirror() ||
      (phase_ == TRAIN && param_.crop_size());
  if (needs_rand) {
    const unsigned int rng_seed = caffe_rng_rand();
    rng_.reset(new Caffe::RNG(rng_seed));
  } else {
    rng_.reset();
  }
}

template <typename Dtype>
int DataTransformer<Dtype>::Rand(int n) {
  CHECK(rng_);
  CHECK_GT(n, 0);
  caffe::rng_t* rng =
      static_cast<caffe::rng_t*>(rng_->generator());
  return ((*rng)() % n);
}
template <typename Dtype>
float DataTransformer<Dtype>::Randfloat(float n, float m) {
  CHECK_GT(m, n);
  float random = float(rand()) / RAND_MAX;
  float result = random * (m - n) + n;
  return result;
}

INSTANTIATE_CLASS(DataTransformer);

}  // namespace caffe
