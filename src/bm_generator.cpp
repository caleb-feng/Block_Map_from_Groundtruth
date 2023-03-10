#include <ros/ros.h>
#include <sensor_msgs/PointCloud2.h>

#include <pcl_ros/point_cloud.h>
#include <pcl_ros/transforms.h>
#include <pcl/common/common.h>
#include <pcl/common/impl/io.hpp>
#include <pcl/io/pcd_io.h>
#include <pcl/kdtree/kdtree_flann.h>
#include <pcl/filters/filter.h>
#include <pcl/filters/voxel_grid.h>
#include <pcl_conversions/pcl_conversions.h>

#include <boost/format.hpp>

#include <iostream>
#include <fstream>
#include <iomanip>
#include <cmath>
#include <vector>
#include <queue>
#include <string>

using namespace std;
using PointType = pcl::PointXYZI;

class bmGenerator
{
public:
    ros::NodeHandle nh;
    ros::Subscriber subCloud;

    // gt poses
    string datasetDIR;
    string gtFileName;
    std::queue<std::pair<double, Eigen::Matrix4d>> gtData;
    Eigen::Matrix4d lastPose;   // record transformation matrix of last frame
    Eigen::Matrix4d prevPose;   // record transformation matrix of previous frame for pose comparison
    Eigen::Matrix4d R_gt_l;

    int numberOfCores;
    int keyCount;

    // map generation and save
    double minTdiff;
    bool firstStamp{true};
    bool updatePrevPoseFlag{true};
    float downSampleValueGlobalMap;
    float downSampleValueBlockMap;
    pcl::VoxelGrid<PointType> downSizeFilterGlobalMap;
    pcl::VoxelGrid<PointType> downSizeFilterBlockMap;
    string gmFileName;
    pcl::PointCloud<PointType>::Ptr globalMap;

    float blockSize;
    int blockID;
    unsigned long long int sumOfPoints;
    pcl::PointCloud<PointType> blockMap;
    vector<pcl::PointCloud<PointType>::Ptr> bmVec;
    pcl::PointCloud<PointType>::Ptr centroidCloud;
    pcl::KdTreeFLANN<PointType>::Ptr centroidKDTree;

public:
    bmGenerator()
    {
        nh.param<std::string>("datasetDIR", datasetDIR, "/shared_volume/bloc_dataset/nclt/20130110/");
        nh.param<std::string>("gtFileName", gtFileName, "gt_2013-01-10.txt");
        nh.param<int>("numberOfCores", numberOfCores, 2);
        nh.param<float>("downSampleValueGlobalMap", downSampleValueGlobalMap, 0.4);
        nh.param<float>("downSampleValueBlockMap", downSampleValueBlockMap, 0.2);
        nh.param<float>("blockSize", blockSize, 50.0);

        initializeParams();

        loadGroundTruth();

        subCloud = nh.subscribe<sensor_msgs::PointCloud2>("/points_raw", 1, &bmGenerator::cloudHandler, this, ros::TransportHints().tcpNoDelay());
    }

    void cloudHandler(const sensor_msgs::PointCloud2ConstPtr& cloudIn)
    {
        if (keyCount < 10)
        {
            keyCount++;
            return;
        }

        if (gtData.empty())
            return;

        double currT = cloudIn->header.stamp.toSec();
        pcl::PointCloud<PointType>::Ptr currPointCloud(new pcl::PointCloud<PointType>());
        pcl::fromROSMsg(*cloudIn, *currPointCloud);

        std::pair<double, Eigen::Matrix4d> currData;
        while (!gtData.empty())
        {
            currData = gtData.front();
            double timeDiff = std::abs(currData.first - currT);
            if (firstStamp) // 'firstStamp' is a flag which denotes the 1st gt retrieved by current pointcloud
            {
                if (updatePrevPoseFlag)
                {
                    prevPose = currData.second; // record the 1st pose for comparison which decides to output BMs
                    updatePrevPoseFlag = false;
                }
                minTdiff = timeDiff;
                lastPose = currData.second;
                firstStamp = false;
            }
            else
            {
                if (timeDiff < minTdiff)
                {
                    minTdiff = timeDiff;
                    lastPose = currData.second;
                } 
                else 
                {
                    saveGlobalMap(currPointCloud);

                    generateBlockMap(currPointCloud);

                    firstStamp = true;
                    break;
                }
            }
            gtData.pop();
        }
        keyCount = 0;
    }

    void initializeParams()
    {
        globalMap.reset(new pcl::PointCloud<PointType>());
        centroidCloud.reset(new pcl::PointCloud<PointType>());
        centroidKDTree.reset(new pcl::KdTreeFLANN<PointType>());
        blockMap.clear();
        gmFileName = std::getenv("HOME") + datasetDIR + "GlobalMap.pcd";
        downSizeFilterGlobalMap.setLeafSize(downSampleValueGlobalMap, downSampleValueGlobalMap, downSampleValueGlobalMap);
        downSizeFilterBlockMap.setLeafSize(downSampleValueBlockMap, downSampleValueBlockMap, downSampleValueBlockMap);
        R_gt_l << 0, -1, 0, 0, -1, 0, 0, 0, 0, 0, -1, 0, 0, 0, 0, 1;
        keyCount = 0;
        blockID = 0;
        sumOfPoints = 0;
    }

    void loadGroundTruth()
    {
        string filedst = std::getenv("HOME") + datasetDIR + gtFileName;
        ifstream filein;
        filein.open(filedst, ios::in);
        if (!filein.is_open())
        {
            ROS_ERROR("can't read the gt file.");
            return;
        }

        double time, tx, ty, tz, qx, qy, qz, qw;
        while(filein >> time >> tx >> ty >> tz >> qx >> qy >> qz >> qw)
        {
            Eigen::Matrix4d tmpTransform = Eigen::Matrix4d::Identity();
            tmpTransform.block<3, 3>(0, 0) = Eigen::Quaterniond(qw, qx, qy, qz).toRotationMatrix();
            tmpTransform.topRightCorner<3, 1>() = Eigen::Vector3d(tx, ty, tz);
            gtData.push(std::pair<double, Eigen::Matrix4d>(time, tmpTransform));
        }
        ROS_INFO("load %ld pose pairs", gtData.size());
    }

    void saveGlobalMap(pcl::PointCloud<PointType>::Ptr cloudIn)
    {
        *globalMap += *transformPointCloud(cloudIn, lastPose);
        pcl::PointCloud<PointType>::Ptr outputCloud(new pcl::PointCloud<PointType>());
        downSizeFilterGlobalMap.setInputCloud(globalMap);
        downSizeFilterGlobalMap.filter(*outputCloud);
        pcl::io::savePCDFileBinary(gmFileName, *outputCloud);
    }

    void saveBlockMap(int saveID)
    {
        boost::format saveFormat("%03d.pcd");
        pcl::PointCloud<PointType>::Ptr outputCloud(new pcl::PointCloud<PointType>());
        downSizeFilterBlockMap.setInputCloud(bmVec.at(saveID));
        downSizeFilterBlockMap.filter(*outputCloud);
        pcl::io::savePCDFileBinary(std::getenv("HOME") + datasetDIR + (saveFormat % saveID).str(), *outputCloud);
        cout << "BM ID: " << saveID << " Cloud size before DS: " << bmVec.at(saveID)->size() << " After DS: " << outputCloud->size() << "\nBMVector: " << endl;
        for (int i = 0; i < bmVec.size(); i++)
        {
            cout << "BM ID: " << i << " Cloud size: " << bmVec.at(i)->size() << endl;
        }
    }

    void generateBlockMap(pcl::PointCloud<PointType>::Ptr cloudIn)
    {
        if (pow((lastPose(0,3) - prevPose(0,3)), 2) + pow((lastPose(1,3) - prevPose(1,3)), 2) + pow((lastPose(3,3) - prevPose(3,3)), 2) < blockSize * blockSize)
        {
            blockMap += *transformPointCloud(cloudIn, lastPose);
        }
        else
        {
            prevPose = lastPose;    // update the 1st pose of a new block map

            Eigen::Vector4d centroid;
            pcl::compute3DCentroid(blockMap, centroid);
            PointType centroidPoint;
            centroidPoint.x = centroid[0];
            centroidPoint.y = centroid[1];
            centroidPoint.z = centroid[2];

            vector<int> pointIdxNKNSearch;
            vector<float> pointDistNKNSearch;
            pcl::PointCloud<PointType>::Ptr tempMap(new pcl::PointCloud<PointType>());
            pcl::copyPointCloud(blockMap, *tempMap);
            if (blockID == 1)   // 2nd block map, no need to check kd-tree
            {
                sumOfPoints += tempMap->size();
                bmVec.push_back(tempMap);
                saveBlockMap(blockID);
                centroidCloud->push_back(centroidPoint);
                centroidKDTree->setInputCloud(centroidCloud);
            }
            else if (blockID > 1)
            {
                pointIdxNKNSearch.clear();
                pointDistNKNSearch.clear();
                centroidKDTree->nearestKSearch(centroidPoint, 1, pointIdxNKNSearch, pointDistNKNSearch);
                float nearestDist = sqrt(pointDistNKNSearch[0]);
                // cout << "nearestDist: " << nearestDist << endl;
                if (nearestDist < 0.5 * blockSize && nearestDist > 0.1 * blockSize)
                {
                    // add overlapping block to old block and recalculate centroid
                    cout << "\nDistance from the nearest BM is " << nearestDist << "m which is shorter than the half of blocksize. BM No." << blockID << " is merged into No." << pointIdxNKNSearch[0] << endl;
                    cout << "before merging, the size of bm: " << bmVec.at(pointIdxNKNSearch[0])->size() << endl;
                    sumOfPoints -= bmVec.at(pointIdxNKNSearch[0])->size();
                    *(bmVec.at(pointIdxNKNSearch[0])) += *tempMap;
                    sumOfPoints += bmVec.at(pointIdxNKNSearch[0])->size();
                    cout << "after merging, the size of bm: " << bmVec.at(pointIdxNKNSearch[0])->size() << endl;
                    saveBlockMap(pointIdxNKNSearch[0]);

                    Eigen::Vector4d newCentroid;
                    pcl::compute3DCentroid(*(bmVec.at(pointIdxNKNSearch[0])), newCentroid);
                    centroidCloud->points[pointIdxNKNSearch[0]].x = newCentroid[0];
                    centroidCloud->points[pointIdxNKNSearch[0]].y = newCentroid[1];
                    centroidCloud->points[pointIdxNKNSearch[0]].z = newCentroid[2];
                    centroidKDTree->setInputCloud(centroidCloud);
                    blockID -= 1;
                }
                else if (nearestDist <= 0.1 * blockSize) 
                {
                    blockID -= 1;
                }
                else 
                {
                    sumOfPoints += tempMap->size();
                    bmVec.push_back(tempMap);
                    saveBlockMap(blockID);
                    centroidCloud->push_back(centroidPoint);
                    centroidKDTree->setInputCloud(centroidCloud);
                }
            }
            else // blockID == 0, 1st block map
            {
                sumOfPoints += tempMap->size();
                bmVec.push_back(tempMap);
                saveBlockMap(blockID);
                centroidCloud->push_back(centroidPoint);
            }
            blockID += 1;

            blockMap.clear();
        }
    }

    pcl::PointCloud<PointType>::Ptr transformPointCloud(pcl::PointCloud<PointType>::Ptr cloudIn, Eigen::Matrix4d transformIn)
    {
        pcl::PointCloud<PointType>::Ptr cloudOut(new pcl::PointCloud<PointType>());
        int cloudSize = cloudIn->size();
        cloudOut->resize(cloudSize);

        // R_w_gt is known. P_l denotes the pointcloud in lidar frame. 
        // R_w_l = R_w_gt * R_gt_l.
        // R_gt_l = [ 0 -1  0 
        //           -1  0  0
        //            0  0 -1]
        // Eigen::Matrix4d transformation = transformIn * R_gt_l;

        #pragma omp parallel for num_threads(numberOfCores)
        for (int i = 0; i < cloudSize; ++i)
        {
            const auto &pointFrom = cloudIn->points[i];

            cloudOut->points[i].x = transformIn(0,0) * pointFrom.x + transformIn(0,1) * pointFrom.y + transformIn(0,2) * pointFrom.z + transformIn(0,3);
            cloudOut->points[i].y = transformIn(1,0) * pointFrom.x + transformIn(1,1) * pointFrom.y + transformIn(1,2) * pointFrom.z + transformIn(1,3);
            cloudOut->points[i].z = transformIn(2,0) * pointFrom.x + transformIn(2,1) * pointFrom.y + transformIn(2,2) * pointFrom.z + transformIn(2,3);
            cloudOut->points[i].intensity = pointFrom.intensity;
        }
        return cloudOut;
    }
};

int main(int argc, char** argv)
{
    ros::init(argc, argv, "bm_generator");
    bmGenerator BM;
    ros::spin();

    // save residual map after ros::shutdown()
    boost::format saveFormat("%03d.pcd");

    pcl::PointCloud<PointType>::Ptr tempMap(new pcl::PointCloud<PointType>());
    pcl::copyPointCloud(BM.blockMap, *tempMap);
    if (tempMap->size() > 0.5 * BM.sumOfPoints / BM.blockID)
    {
        pcl::PointCloud<PointType>::Ptr outputCloud(new pcl::PointCloud<PointType>());
        BM.downSizeFilterBlockMap.setInputCloud(tempMap);
        BM.downSizeFilterBlockMap.filter(*outputCloud);
        pcl::io::savePCDFileBinary(std::getenv("HOME") + BM.datasetDIR + (saveFormat % BM.blockID).str(), *outputCloud);

        Eigen::Vector4d centroid;
        pcl::compute3DCentroid(BM.blockMap, centroid);
        PointType centroidPoint;
        centroidPoint.x = centroid[0];
        centroidPoint.y = centroid[1];
        centroidPoint.z = centroid[2];
        BM.centroidCloud->push_back(centroidPoint);
    }
    pcl::io::savePCDFileBinary(std::getenv("HOME") + BM.datasetDIR + "CentroidCloud.pcd", *(BM.centroidCloud));

    return 0;
}