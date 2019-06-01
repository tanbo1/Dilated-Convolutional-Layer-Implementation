#include "dilated_convolutional_layer.h"
#include "convolutional_layer.h"
#include "utils.h"
#include "batchnorm_layer.h"
#include "blas.h"
#include "gemm.h"
#include <stdio.h>
#include <time.h>

#ifdef AI2
#include "xnor_layer.h"
#endif

void binarize_cpu(float* input, int n, float* binary);

int dilated_conv_out_height(dilated_convolutional_layer l)
{
    int dsize = (l.dilate_rate - 1) * (l.size + 1) + l.size;
    //printf("new kernel size = %d\n", l.size);
    return (l.h + 2*l.pad - dsize) / l.stride + 1;
}

int dilated_conv_out_width(dilated_convolutional_layer l)
{
    int dsize = (l.dilate_rate - 1) * (l.size + 1) + l.size;
    return (l.w + 2*l.pad - dsize) / l.stride + 1;
}

image get_dilated_conv_image(dilated_convolutional_layer l)
{
    return float_to_image(l.out_w,l.out_h,l.out_c,l.output);
}

image get_dilated_conv_delta(dilated_convolutional_layer l)
{
    return float_to_image(l.out_w,l.out_h,l.out_c,l.delta);
}

static size_t get_workspace_size(layer l){
#ifdef CUDNN
    if(gpu_index >= 0){
        size_t most = 0;
        size_t s = 0;
        cudnnGetConvolutionForwardWorkspaceSize(cudnn_handle(),
                l.srcTensorDesc,
                l.weightDesc,
                l.convDesc,
                l.dstTensorDesc,
                l.fw_algo,
                &s);
        if (s > most) most = s;
        cudnnGetConvolutionBackwardFilterWorkspaceSize(cudnn_handle(),
                l.srcTensorDesc,
                l.ddstTensorDesc,
                l.convDesc,
                l.dweightDesc,
                l.bf_algo,
                &s);
        if (s > most) most = s;
        cudnnGetConvolutionBackwardDataWorkspaceSize(cudnn_handle(),
                l.weightDesc,
                l.ddstTensorDesc,
                l.convDesc,
                l.dsrcTensorDesc,
                l.bd_algo,
                &s);
        if (s > most) most = s;
        return most;
    }
#endif
    return (size_t)l.out_h*l.out_w*l.size*l.size*l.c/l.groups*sizeof(float);
}

#ifdef GPU
#ifdef CUDNN
void cudnn_convolutional_setup(layer *l)
{
    cudnnSetTensor4dDescriptor(l->dsrcTensorDesc, CUDNN_TENSOR_NCHW, CUDNN_DATA_FLOAT, l->batch, l->c, l->h, l->w); 
    cudnnSetTensor4dDescriptor(l->ddstTensorDesc, CUDNN_TENSOR_NCHW, CUDNN_DATA_FLOAT, l->batch, l->out_c, l->out_h, l->out_w); 

    cudnnSetTensor4dDescriptor(l->srcTensorDesc, CUDNN_TENSOR_NCHW, CUDNN_DATA_FLOAT, l->batch, l->c, l->h, l->w); 
    cudnnSetTensor4dDescriptor(l->dstTensorDesc, CUDNN_TENSOR_NCHW, CUDNN_DATA_FLOAT, l->batch, l->out_c, l->out_h, l->out_w); 
    cudnnSetTensor4dDescriptor(l->normTensorDesc, CUDNN_TENSOR_NCHW, CUDNN_DATA_FLOAT, 1, l->out_c, 1, 1); 

    cudnnSetFilter4dDescriptor(l->dweightDesc, CUDNN_DATA_FLOAT, CUDNN_TENSOR_NCHW, l->n, l->c/l->groups, l->size, l->size); 
    cudnnSetFilter4dDescriptor(l->weightDesc, CUDNN_DATA_FLOAT, CUDNN_TENSOR_NCHW, l->n, l->c/l->groups, l->size, l->size); 
    #if CUDNN_MAJOR >= 6
    cudnnSetConvolution2dDescriptor(l->convDesc, l->pad, l->pad, l->stride, l->stride, 1, 1, CUDNN_CROSS_CORRELATION, CUDNN_DATA_FLOAT);
    #else
    cudnnSetConvolution2dDescriptor(l->convDesc, l->pad, l->pad, l->stride, l->stride, 1, 1, CUDNN_CROSS_CORRELATION);
    #endif

    #if CUDNN_MAJOR >= 7
    cudnnSetConvolutionGroupCount(l->convDesc, l->groups);
    #else
    if(l->groups > 1){
        error("CUDNN < 7 doesn't support groups, please upgrade!");
    }
    #endif

    cudnnGetConvolutionForwardAlgorithm(cudnn_handle(),
            l->srcTensorDesc,
            l->weightDesc,
            l->convDesc,
            l->dstTensorDesc,
            CUDNN_CONVOLUTION_FWD_SPECIFY_WORKSPACE_LIMIT,
            4000000000,
            &l->fw_algo);
    cudnnGetConvolutionBackwardDataAlgorithm(cudnn_handle(),
            l->weightDesc,
            l->ddstTensorDesc,
            l->convDesc,
            l->dsrcTensorDesc,
            CUDNN_CONVOLUTION_BWD_DATA_SPECIFY_WORKSPACE_LIMIT,
            4000000000,
            &l->bd_algo);
    cudnnGetConvolutionBackwardFilterAlgorithm(cudnn_handle(),
            l->srcTensorDesc,
            l->ddstTensorDesc,
            l->convDesc,
            l->dweightDesc,
            CUDNN_CONVOLUTION_BWD_FILTER_SPECIFY_WORKSPACE_LIMIT,
            4000000000,
            &l->bf_algo);
}
#endif
#endif

dilated_convolutional_layer make_dilated_conv_layer(int batch, int h, int w, int c, int n, int groups, int size, int stride, int padding, ACTIVATION activation, int batch_normalize, int binary, int xnor, int adam, int dilate_rate)
{
    int i;
    dilated_convolutional_layer l = {0};
    l.type = DILATED_CONVOLUTIONAL;

    l.dilate_rate = dilate_rate;
    l.groups = groups;
    l.h = h;
    l.w = w;
    l.c = c;
    l.n = n;
    l.binary = binary;
    l.xnor = xnor;
    l.batch = batch;
    l.stride = stride;
    l.size = size;
    l.pad = padding;
    l.batch_normalize = batch_normalize;

    l.weights = calloc(c/groups*n*size*size, sizeof(float));
    l.weight_updates = calloc(c/groups*n*size*size, sizeof(float));

    l.biases = calloc(n, sizeof(float));
    l.bias_updates = calloc(n, sizeof(float));

    l.nweights = c/groups*n*size*size;
    l.nbiases = n;

    float scale = sqrt(2./(size*size*c/l.groups));
    
    for(i = 0; i < l.nweights; ++i) l.weights[i] = scale*rand_normal();
    int out_w = dilated_conv_out_width(l);
    int out_h = dilated_conv_out_height(l);
    l.out_h = out_h;
    l.out_w = out_w;
    l.out_c = n;
    l.outputs = l.out_h * l.out_w * l.out_c;
    l.inputs = l.w * l.h * l.c;

    l.output = calloc(l.batch*l.outputs, sizeof(float));
    l.delta  = calloc(l.batch*l.outputs, sizeof(float));

    l.forward = forward_dilated_conv_layer;
    l.backward = backward_dilated_conv_layer;
    l.update = update_dilated_conv_layer;
    if(binary){
        l.binary_weights = calloc(l.nweights, sizeof(float));
        l.cweights = calloc(l.nweights, sizeof(char));
        l.scales = calloc(n, sizeof(float));
    }
    if(xnor){
        l.binary_weights = calloc(l.nweights, sizeof(float));
        l.binary_input = calloc(l.inputs*l.batch, sizeof(float));
    }

    if(batch_normalize){
        l.scales = calloc(n, sizeof(float));
        l.scale_updates = calloc(n, sizeof(float));
        for(i = 0; i < n; ++i){
            l.scales[i] = 1;
        }

        l.mean = calloc(n, sizeof(float));
        l.variance = calloc(n, sizeof(float));

        l.mean_delta = calloc(n, sizeof(float));
        l.variance_delta = calloc(n, sizeof(float));

        l.rolling_mean = calloc(n, sizeof(float));
        l.rolling_variance = calloc(n, sizeof(float));
        l.x = calloc(l.batch*l.outputs, sizeof(float));
        l.x_norm = calloc(l.batch*l.outputs, sizeof(float));
    }
    if(adam){
        l.m = calloc(l.nweights, sizeof(float));
        l.v = calloc(l.nweights, sizeof(float));
        l.bias_m = calloc(n, sizeof(float));
        l.scale_m = calloc(n, sizeof(float));
        l.bias_v = calloc(n, sizeof(float));
        l.scale_v = calloc(n, sizeof(float));
    }

#ifdef GPU
    l.forward_gpu = forward_dilated_conv_layer_gpu;
    l.backward_gpu = backward_dilated_conv_layer_gpu;
    l.update_gpu = update_dilated_conv_layer_gpu;

    if(gpu_index >= 0){
        if (adam) {
            l.m_gpu = cuda_make_array(l.m, l.nweights);
            l.v_gpu = cuda_make_array(l.v, l.nweights);
            l.bias_m_gpu = cuda_make_array(l.bias_m, n);
            l.bias_v_gpu = cuda_make_array(l.bias_v, n);
            l.scale_m_gpu = cuda_make_array(l.scale_m, n);
            l.scale_v_gpu = cuda_make_array(l.scale_v, n);
        }

        l.weights_gpu = cuda_make_array(l.weights, l.nweights);
        l.weight_updates_gpu = cuda_make_array(l.weight_updates, l.nweights);

        l.biases_gpu = cuda_make_array(l.biases, n);
        l.bias_updates_gpu = cuda_make_array(l.bias_updates, n);

        l.delta_gpu = cuda_make_array(l.delta, l.batch*out_h*out_w*n);
        l.output_gpu = cuda_make_array(l.output, l.batch*out_h*out_w*n);

        if(binary){
            l.binary_weights_gpu = cuda_make_array(l.weights, l.nweights);
        }
        if(xnor){
            l.binary_weights_gpu = cuda_make_array(l.weights, l.nweights);
            l.binary_input_gpu = cuda_make_array(0, l.inputs*l.batch);
        }

        if(batch_normalize){
            l.mean_gpu = cuda_make_array(l.mean, n);
            l.variance_gpu = cuda_make_array(l.variance, n);

            l.rolling_mean_gpu = cuda_make_array(l.mean, n);
            l.rolling_variance_gpu = cuda_make_array(l.variance, n);

            l.mean_delta_gpu = cuda_make_array(l.mean, n);
            l.variance_delta_gpu = cuda_make_array(l.variance, n);

            l.scales_gpu = cuda_make_array(l.scales, n);
            l.scale_updates_gpu = cuda_make_array(l.scale_updates, n);

            l.x_gpu = cuda_make_array(l.output, l.batch*out_h*out_w*n);
            l.x_norm_gpu = cuda_make_array(l.output, l.batch*out_h*out_w*n);
        }
#ifdef CUDNN
        cudnnCreateTensorDescriptor(&l.normTensorDesc);
        cudnnCreateTensorDescriptor(&l.srcTensorDesc);
        cudnnCreateTensorDescriptor(&l.dstTensorDesc);
        cudnnCreateFilterDescriptor(&l.weightDesc);
        cudnnCreateTensorDescriptor(&l.dsrcTensorDesc);
        cudnnCreateTensorDescriptor(&l.ddstTensorDesc);
        cudnnCreateFilterDescriptor(&l.dweightDesc);
        cudnnCreateConvolutionDescriptor(&l.convDesc);
        cudnn_convolutional_setup(&l);
#endif
    }
#endif
    l.workspace_size = get_workspace_size(l);
    l.activation = activation;

    fprintf(stderr, "dilated_conv  %5d %2d x%2d /%2d  %4d x%4d x%4d   ->  %4d x%4d x%4d  %5.3f BFLOPs\n", n, size, size, stride, w, h, c, l.out_w, l.out_h, l.out_c, (2.0 * l.n * l.size*l.size*l.c/l.groups * l.out_h*l.out_w)/1000000000.);

    return l;
}

void denormalize_dilated_conv_layer(dilated_convolutional_layer l)
{
    int i, j;
    for(i = 0; i < l.n; ++i){
        float scale = l.scales[i]/sqrt(l.rolling_variance[i] + .00001);
        for(j = 0; j < l.c/l.groups*l.size*l.size; ++j){
            l.weights[i*l.c/l.groups*l.size*l.size + j] *= scale;
        }
        l.biases[i] -= l.rolling_mean[i] * scale;
        l.scales[i] = 1;
        l.rolling_mean[i] = 0;
        l.rolling_variance[i] = 1;
    }
}


void resize_dilated_conv_layer(dilated_convolutional_layer *l, int w, int h)
{
    l->w = w;
    l->h = h;
    int out_w = dilated_conv_out_width(*l);
    int out_h = dilated_conv_out_height(*l);

    l->out_w = out_w;
    l->out_h = out_h;

    l->outputs = l->out_h * l->out_w * l->out_c;
    l->inputs = l->w * l->h * l->c;

    l->output = realloc(l->output, l->batch*l->outputs*sizeof(float));
    l->delta  = realloc(l->delta,  l->batch*l->outputs*sizeof(float));
    if(l->batch_normalize){
        l->x = realloc(l->x, l->batch*l->outputs*sizeof(float));
        l->x_norm  = realloc(l->x_norm, l->batch*l->outputs*sizeof(float));
    }

#ifdef GPU
    cuda_free(l->delta_gpu);
    cuda_free(l->output_gpu);

    l->delta_gpu =  cuda_make_array(l->delta,  l->batch*l->outputs);
    l->output_gpu = cuda_make_array(l->output, l->batch*l->outputs);

    if(l->batch_normalize){
        cuda_free(l->x_gpu);
        cuda_free(l->x_norm_gpu);

        l->x_gpu = cuda_make_array(l->output, l->batch*l->outputs);
        l->x_norm_gpu = cuda_make_array(l->output, l->batch*l->outputs);
    }
#ifdef CUDNN
    cudnn_convolutional_setup(l);
#endif
#endif
    l->workspace_size = get_workspace_size(*l);
}

void add_bias_dilated(float *output, float *biases, int batch, int n, int size)
{
    int i,j,b;
    for(b = 0; b < batch; ++b){
        for(i = 0; i < n; ++i){
            for(j = 0; j < size; ++j){
                output[(b*n + i)*size + j] += biases[i];
            }
        }
    }
}
void scale_bias_dilated(float *output, float *scales, int batch, int n, int size)
{
    int i,j,b;
    for(b = 0; b < batch; ++b){
        for(i = 0; i < n; ++i){
            for(j = 0; j < size; ++j){
                output[(b*n + i)*size + j] *= scales[i];
            }
        }
    }
}

void backward_bias_dilated(float *bias_updates, float *delta, int batch, int n, int size)
{
    int i,b;
    for(b = 0; b < batch; ++b){
        for(i = 0; i < n; ++i){
            bias_updates[i] += sum_array(delta+size*(i+b*n), size);
        }
    }
}


void forward_dilated_conv_layer(dilated_convolutional_layer l, network net)
{
    int i, j;
    fill_cpu(l.outputs*l.batch, 0, l.output, 1);

    if(l.xnor){                                                                              // XNor-Net architecture 
        binarize_weights(l.weights, l.n, l.c/l.groups*l.size*l.size, l.binary_weights);      // binarilize weight
        swap_binary(&l);                                                                     // swap weight & binary_weight
        binarize_cpu(net.input, l.c*l.h*l.w*l.batch, l.binary_input);                        // binarilize input
        net.input = l.binary_input;
    }

    int m = l.n/l.groups;                                // 每组的kernel个数
    int k = l.size*l.size*l.c/l.groups;                  // 每组kernel中元素的个数
    int n = l.out_w*l.out_h;                             // 输出图像每个channel的像素个数
    for(i = 0; i < l.batch; ++i){
    //大循环，batch是一组图片，循环内每次对一张图片卷积
        for(j = 0; j < l.groups; ++j){
        //小循环，每次使用一组weights对一张图像进行卷积
            float *a = l.weights + j*l.nweights/l.groups;   // 第j组第一个卷积核的开头元素
            float *b = net.workspace;                       // re-formated image data
            float *c = l.output + (i*l.groups + j)*n*m;     // 第i个图像在和第j组kernel卷积时输出元素的存放位置
            float *im =  net.input + (i*l.groups + j)*l.c/l.groups*l.h*l.w;    // input data

            if (l.size == 1) {
                b = im;
            } else {
                im2col_dilated_cpu(im, l.c/l.groups, l.h, l.w, l.size, l.stride, l.pad, b, l.dilate_rate); // re-format the input image
            }
            gemm(0,0,m,n,k,1,a,k,b,n,1,c,n);
        }
    }

    if(l.batch_normalize){
        forward_batchnorm_layer(l, net);
    } else {
        add_bias(l.output, l.biases, l.batch, l.n, l.out_h*l.out_w);
    }

    activate_array(l.output, l.outputs*l.batch, l.activation);
    if(l.binary || l.xnor) swap_binary(&l);
}


void backward_dilated_conv_layer(dilated_convolutional_layer l, network net)
{
    int i, j;
    int m = l.n/l.groups;
    int n = l.size*l.size*l.c/l.groups;
    int k = l.out_w*l.out_h;

    gradient_array(l.output, l.outputs*l.batch, l.activation, l.delta);

    if(l.batch_normalize){
        backward_batchnorm_layer(l, net);
    } else {
        backward_bias(l.bias_updates, l.delta, l.batch, l.n, k);
    }

    for(i = 0; i < l.batch; ++i){
        for(j = 0; j < l.groups; ++j){
            float *a = l.delta + (i*l.groups + j)*m*k;        
            float *b = net.workspace;                          
            float *c = l.weight_updates + j*l.nweights/l.groups;

            float *im  = net.input + (i*l.groups + j)*l.c/l.groups*l.h*l.w;
            float *imd = net.delta + (i*l.groups + j)*l.c/l.groups*l.h*l.w;

            if(l.size == 1){
                b = im;
            } else {
                im2col_dilated_cpu(im, l.c/l.groups, l.h, l.w, 
                        l.size, l.stride, l.pad, b, l.dilate_rate);
            }

            gemm(0,1,m,n,k,1,a,k,b,k,1,c,n);    // c (weight_update) = x (*) dL/dh

            if (net.delta) { // 如果上一层的delta已经动态分配了内存  net.delta是前层的导数，l.delta是本层的导数
                a = l.weights + j*l.nweights/l.groups;  // a = weight matrix
                b = l.delta + (i*l.groups + j)*m*k;     // b = delta matrix
                c = net.workspace;                      // c = workspace
                if (l.size == 1) {
                    c = imd;
                }

                gemm(1,0,n,k,m,1,a,n,b,k,0,c,k);       // workspace = weight matrix' * delta  matrix

                /*printf("CPU input of col2im_dilated = \n");
                for (int i=0; i<n; i++){
                    for (int j=0; j<k; j++){
                        printf("%d ",(int)c[i*k+j]);
                    }printf("\n");
                }printf("\n");*/


                if (l.size != 1) {
                    col2im_dilated_cpu(net.workspace, l.c/l.groups, l.h, l.w, l.size, l.stride, l.pad, l.dilate_rate, imd);
                    // input: workspace, output: imd(net.delta)
                
                /*printf("CPU output of col2im_dilated = \n");
                for (int i=0; i<l.h*l.c; i++){
                    for (int j=0; j<l.w; j++){
                        printf("%f\t",imd[i*l.w+j]);
                    }printf("\n");
                }printf("\n");*/
                }
            }
        }
    }
}

void update_dilated_conv_layer(dilated_convolutional_layer l, update_args a)
{
    float learning_rate = a.learning_rate*l.learning_rate_scale;
    float momentum = a.momentum;
    float decay = a.decay;
    int batch = a.batch;

    axpy_cpu(l.n, learning_rate/batch, l.bias_updates, 1, l.biases, 1);
    scal_cpu(l.n, momentum, l.bias_updates, 1);

    if(l.scales){
        axpy_cpu(l.n, learning_rate/batch, l.scale_updates, 1, l.scales, 1);
        scal_cpu(l.n, momentum, l.scale_updates, 1);
    }

    axpy_cpu(l.nweights, -decay*batch, l.weights, 1, l.weight_updates, 1);
    axpy_cpu(l.nweights, learning_rate/batch, l.weight_updates, 1, l.weights, 1);
    scal_cpu(l.nweights, momentum, l.weight_updates, 1);
}


image get_dilated_conv_weight(dilated_convolutional_layer l, int i)
{
    int h = l.size;
    int w = l.size;
    int c = l.c/l.groups;
    return float_to_image(w,h,c,l.weights+i*h*w*c);
}

void test_dconv_forward_cpu()
{
    
    int batch = 100;
    int h = 32;
    int w = 32;
    int c = 3;
    int n = 32;
    int groups = 1;
    int size = 5;
    int stride = 1;
    int padding = 5;
    ACTIVATION activation = LEAKY;
    int batch_normalize = 0;
    int binary = 0;
    int xnor = 0;
    int adam = 0;
    int dilate_rate = 2;
    
    dilated_convolutional_layer l = make_dilated_conv_layer(
        batch,h,w,c,n,groups,size,stride,padding,activation, batch_normalize, binary, xnor, adam, dilate_rate);
    
    network net = *make_network(1);
    net.layers = &l;

	net.input = (float*) calloc (batch*h*w*c, sizeof(float));
	l.weights = (float*) calloc (size*size*c*n, sizeof(float));
    l.output = (float*) calloc (batch*l.out_c*l.out_h*l.out_w, sizeof(float));
    net.workspace = (float*) calloc (l.workspace_size, sizeof(float));
    
    FILE *fp;
	    if((fp=fopen("caffe_forward_input.txt","r"))==NULL){
			printf("Open file caffe_forward_input failed.\n");
			exit(0);
		}

		for(int i=0; i<h*w*c*batch; i++){
			fscanf(fp,"%f,", &net.input[i]);
		}
		fclose(fp);


		FILE *fin;
		if ((fin = fopen("caffe_forward_weights.txt","r"))==NULL){
			printf("Open file caffe_forward_weights failed.\n");
			exit(0);
		}
		//fscanf(fin, "%*[^\n]\n", NULL,NULL);
		for(int i=0; i<size*size*c*n; i++){
			fscanf(fin, "%f,", &l.weights[i]);
		}
		fclose(fin);
    printf("finish reading all inputs.\n");

   
    forward_dilated_conv_layer(l, net);

    printf("forward dconv gpu complete.\n");

    
    FILE *f3;
	if((f3 = fopen("darknet_output.txt", "a"))==NULL){
		printf("Error opening file darknet_output\n");
		exit(0);
	}
	for (int i=0; i<l.out_c*l.out_h*l.out_w*batch; i++){
		fprintf(f3, "%e, ", l.output[i]);
		if (i%10 == 9) fprintf(f3,"\n");
	}
    fclose(f3);
    
    printf("test completed successfully.\n");
}

void test_new_dconv_forward_cpu()
{
    int batch = 1;
    int h = 9;
    int w = 9;
    int c = 1;
    int n = 1;
    int groups = 1;
    int size = 3;
    int stride = 2;
    int padding = 0;
    ACTIVATION activation = LEAKY;
    int batch_normalize = 0;
    int binary = 0;
    int xnor = 0;
    int adam = 0;
    int dilate_rate = 2;
    
    dilated_convolutional_layer l = make_dilated_conv_layer(
        batch,h,w,c,n,groups,size,stride,padding,activation, batch_normalize, binary, xnor, adam, dilate_rate);
    
    network net = *make_network(1);
    net.layers = &l;

	net.input = (float*) calloc (batch*h*w*c, sizeof(float));
	l.weights = (float*) calloc (size*size*c*n, sizeof(float));
    l.output = (float*) calloc (batch*l.out_c*l.out_h*l.out_w, sizeof(float));
    net.workspace = (float*) calloc (l.workspace_size, sizeof(float));

    int num = 1;
    for (int i = 0; i < 9; i++)
    {
        for (int j = 0; j < 9; j++)
        {
            net.input[i + 9 * j] = num;
            num++;
        }   
    }

    for (int i = 0; i< 9; i++)
    {
        l.weights[i] = 1;
    }

   
    forward_dilated_conv_layer(l, net);

    printf("forward dconv cpu complete.\n");

    
    printf("test completed successfully.\n");
}


void test_dconv_backprop_cpu()
{
    
    int batch = 100;
    int h = 8;
    int w = 8;
    int c = 32;
    int n = 64;
    int groups = 1;
    int size = 5;
    int stride = 1;
    int padding = 5;
    ACTIVATION activation = LEAKY;
    int batch_normalize = 0;
    int binary = 0;
    int xnor = 0;
    int adam = 0;
    int dilate_rate = 2;
    
    
    dilated_convolutional_layer l = make_dilated_conv_layer(
        batch,h,w,c,n,groups,size,stride,padding,activation, batch_normalize, binary, xnor, adam, dilate_rate);
    
    network net = *make_network(1);
    net.layers = &l;

	net.input = (float*) calloc (batch*h*w*c, sizeof(float));
	l.weights = (float*) calloc (size*size*c*n, sizeof(float));
	l.delta = (float*) calloc (batch*l.out_w*l.out_h*l.out_c, sizeof(float));
	net.delta = (float*) calloc (batch*h*w*c, sizeof(float));
    l.weight_updates = (float*) calloc (size*size*c*n, sizeof(float));
    net.workspace = (float*) calloc (l.workspace_size, sizeof(float));
    
    FILE *fp;
	    if((fp=fopen("caffe_backprop_input.txt","r"))==NULL){
			printf("Open file caffe_backprop_input failed.\n");
			exit(0);
		}

		for(int i=0; i<h*w*c*batch; i++){
			fscanf(fp,"%f,", &net.input[i]);
		}
		fclose(fp);


		FILE *fin;
		if ((fin = fopen("caffe_backprop_weights.txt","r"))==NULL){
			printf("Open file caffe_backprop_weights failed.\n");
			exit(0);
		}
		//fscanf(fin, "%*[^\n]\n", NULL,NULL);
		for(int i=0; i<size*size*c*n; i++){
			fscanf(fin, "%f,", &l.weights[i]);
		}
		fclose(fin);

		FILE *f1;
		if ((f1 = fopen("caffe_backprop_topdiff.txt","r"))==NULL){
			printf("Open file caffe_backprop_topdiff.txt failed.\n");
			exit(0);
		}
		for (int i=0; i<l.out_w*l.out_h*l.out_c*batch; i++){
			fscanf(f1, "%f,", &l.delta[i]);
		}
        fclose(f1);
    printf("finish reading all inputs.\n");

    

    //forward_dilated_conv_layer_gpu(l, net);

    //printf("forward dconv gpu complete.\n");


    


    backward_dilated_conv_layer(l,net);
    printf("backprop dconv gpu complete.\n");


    FILE *f;
	if((f = fopen("darknet_weight_diff.txt", "a"))==NULL){
		printf("Error opening file weight_diff\n");
		exit(0);
	}
	for (int i=0; i<size*size*n*c; i++){
		fprintf(f,"%e,",l.weight_updates[i]);
		if (i%10 == 9) fprintf(f,"\n");
	}
	fclose(f);

	FILE *f2;
	if((f2 = fopen("darknet_bottom_diff.txt", "a"))==NULL){
		printf("Error opening file bottom_diff\n");
		exit(0);
	}
	for (int i=0; i<h*w*c*batch; i++){
		fprintf(f2, "%e, ", net.delta[i]);
		if (i%10 == 9) fprintf(f2,"\n");
	}
    fclose(f2);
    
    printf("test completed successfully.\n");
}



void rgbgr_weights_dilated(dilated_convolutional_layer l)
{
    int i;
    for(i = 0; i < l.n; ++i){
        image im = get_convolutional_weight(l, i);
        if (im.c == 3) {
            rgbgr_image(im);
        }
    }
}

void rescale_weights_dilated(dilated_convolutional_layer l, float scale, float trans)
{
    int i;
    for(i = 0; i < l.n; ++i){
        image im = get_convolutional_weight(l, i);
        if (im.c == 3) {
            scale_image(im, scale);
            float sum = sum_array(im.data, im.w*im.h*im.c);
            l.biases[i] += sum*trans;
        }
    }
}

image *get_weights_dilated(dilated_convolutional_layer l)
{
    image *weights = calloc(l.n, sizeof(image));
    int i;
    for(i = 0; i < l.n; ++i){
        weights[i] = copy_image(get_convolutional_weight(l, i));
        normalize_image(weights[i]);
        /*
           char buff[256];
           sprintf(buff, "filter%d", i);
           save_image(weights[i], buff);
         */
    }
    //error("hey");
    return weights;
}

image *visualize_dilated_conv_layer(dilated_convolutional_layer l, char *window, image *prev_weights)
{
    image *single_weights = get_weights(l);
    show_images(single_weights, l.n, window);

    image delta = get_convolutional_image(l);
    image dc = collapse_image_layers(delta, 1);
    char buff[256];
    sprintf(buff, "%s: Output", window);
    //show_image(dc, buff);
    //save_image(dc, buff);
    free_image(dc);
    return single_weights;
}