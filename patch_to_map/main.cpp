//
//  main.cpp
//  patch_to_map Prototype code to write geographic rasters from data stored in a Cassandra database.
//
//  Created by Brian C. Miles on 5/17/15.
//  Copyright (c) 2015 selimnairb. All rights reserved.
//
/*
 
 To run:
 
./patch_to_map 192.168.1.3 patch_all_session_2_world_itr_424 /Users/miles/Desktop/scratch/patchdb/Coweeta-custom/basin.tiff /Users/miles/Desktop/scratch/patchdb/Coweeta-custom/hillslopes.tiff /Users/miles/Desktop/scratch/patchdb/Coweeta-custom/patch.tiff /Users/miles/Desktop/scratch/patchdb/Coweeta-custom/patch.tiff output-19850102.tiff sat_def_z 1985-01-02
 */
#include <iostream>

#include <string>
#include <map>

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#include "gdal_priv.h"
#include "cpl_conv.h"

#include "cassandra.h"

using namespace std;

#define DEFAULT_BAND 1

int _read_raster(const char *rast_fn, GDALDataset **rast,
                 int outSizeX, int outSizeY, double outPixelHeight, double outPixelWidth,
                 const char *outProjWkt) {
    double tmpGeoTransform[6];
    *rast = (GDALDataset *) GDALOpen(rast_fn, GA_ReadOnly);
    if(*rast == NULL) {
        printf("Failed to open %s\n", rast_fn);
        return EXIT_FAILURE;
    }
    if((*rast)->GetGeoTransform(tmpGeoTransform) != CE_None) {
        printf("Unable to fetch geographic transform information from raster %s\n", rast_fn);
        return EXIT_FAILURE;
    }
    // Make sure the raster is the same resolution, extent, and spatial reference as the reference raster
    assert(tmpGeoTransform[5] == outPixelHeight);
    assert(tmpGeoTransform[1] == outPixelWidth);
    assert((*rast)->GetRasterXSize() == outSizeX);
    assert((*rast)->GetRasterYSize() == outSizeY);
    assert(strcmp((*rast)->GetProjectionRef(), outProjWkt) == 0);
    
    return EXIT_SUCCESS;
}

void _index_raster(map<string, pair<int, int>> &raster_index, int nXSize, int nYSize,
                   GDALDataset  *basinDataset, GDALDataset  *hillsDataset,
                   GDALDataset  *zoneDataset, GDALDataset  *patchDataset) {
    GDALRasterBand *basin = basinDataset->GetRasterBand(1);
    GDALDataType basin_t = basin->GetRasterDataType();
    assert(basin_t == GDT_Byte ||
           basin_t == GDT_UInt16 ||
           basin_t == GDT_Int16 ||
           basin_t == GDT_UInt32 ||
           basin_t == GDT_Int32);
    uint32_t basinND = (uint32_t)basin->GetNoDataValue();
    GDALRasterBand *hills = hillsDataset->GetRasterBand(1);
    GDALDataType hills_t = hills->GetRasterDataType();
    assert(hills_t == GDT_Byte ||
           hills_t == GDT_UInt16 ||
           hills_t == GDT_Int16 ||
           hills_t == GDT_UInt32 ||
           hills_t == GDT_Int32);
    uint32_t hillsND = (uint32_t)basin->GetNoDataValue();
    GDALRasterBand *zone = zoneDataset->GetRasterBand(1);
    GDALDataType zone_t = zone->GetRasterDataType();
    assert(zone_t == GDT_Byte ||
           zone_t == GDT_UInt16 ||
           zone_t == GDT_Int16 ||
           zone_t == GDT_UInt32 ||
           zone_t == GDT_Int32);
    uint32_t zoneND = (uint32_t)basin->GetNoDataValue();
    GDALRasterBand *patch = patchDataset->GetRasterBand(1);
    GDALDataType patch_t = patch->GetRasterDataType();
    assert(patch_t == GDT_Byte ||
           patch_t == GDT_UInt16 ||
           patch_t == GDT_Int16 ||
           patch_t == GDT_UInt32 ||
           patch_t == GDT_Int32);
    uint32_t patchND = (uint32_t)basin->GetNoDataValue();
    
    uint32_t *basinScanline;
    basinScanline = (uint32_t *) CPLMalloc(sizeof(uint32_t) * nXSize);
    
    uint32_t *hillsScanline;
    hillsScanline = (uint32_t *) CPLMalloc(sizeof(uint32_t)*nXSize);
    
    uint32_t *zoneScanline;
    zoneScanline = (uint32_t *) CPLMalloc(sizeof(uint32_t)*nXSize);
    
    uint32_t *patchScanline;
    patchScanline = (uint32_t *) CPLMalloc(sizeof(uint32_t)*nXSize);
    
    char key[64];
    
    for (int y = 0; y < nYSize; y++) {
        basin->RasterIO(GF_Read, 0, y, nXSize, 1,
                        basinScanline, nXSize, 1, GDT_UInt32,
                        0, 0);
        hills->RasterIO(GF_Read, 0, y, nXSize, 1,
                        hillsScanline, nXSize, 1, GDT_UInt32,
                        0, 0);
        zone->RasterIO(GF_Read, 0, y, nXSize, 1,
                        zoneScanline, nXSize, 1, GDT_UInt32,
                        0, 0);
        patch->RasterIO(GF_Read, 0, y, nXSize, 1,
                        patchScanline, nXSize, 1, GDT_UInt32,
                        0, 0);
        
        for (int x = 0; x < nXSize; x++) {
            // Ignore NoData
            if (basinScanline[x] == basinND || hillsScanline[x] == hillsND ||
                zoneScanline[x] == zoneND || patchScanline[x] == patchND) {
                continue;
            }
            snprintf(key, 64, "%d:%d:%d:%d",
                     basinScanline[x], hillsScanline[x], zoneScanline[x], patchScanline[x]);
            //printf("Key: %s\n", key);
            string key_as_str = (string)key;
            raster_index[key_as_str] = pair<int, int>(x, y);
        }
    }
}

int main(int argc, char **argv) {
    
    assert(argc == 10);
    char *cass_host = argv[1];
    char *cass_keyspace = argv[2];
    
    char *basin_fn = argv[3];
    char *hills_fn = argv[4];
    char *zone_fn = argv[5];
    char *patch_fn = argv[6];
    
    char* dest_fn = argv[7];
    char* variable = argv[8];
    char* date = argv[9];
    
    printf("Cassandra host: %s, keyspace: %s\n", cass_host, cass_keyspace);
    printf("Basin raster: %s\n", basin_fn);
    printf("Hillslope raster: %s\n", hills_fn);
    printf("Zone raster: %s\n", zone_fn);
    printf("Patch raster: %s\n", patch_fn);
    
    GDALDataset  *basinDataset;
    GDALDataset  *hillsDataset;
    GDALDataset  *zoneDataset;
    GDALDataset  *patchDataset;
    
    int outSizeX, outSizeY;
    const char *outProjWkt;
    double outGeoTransform[6];
    double outPixelHeight, outPixelWidth;
    int result;
  
    map<string, pair<int, int>> patch_index;
    map<int, map<int,double>> raster_index;
    
    GDALAllRegister();
    
    basinDataset = (GDALDataset *) GDALOpen(basin_fn, GA_ReadOnly);
    if(basinDataset == NULL) {
        printf("Failed to open %s\n", basin_fn);
        return EXIT_FAILURE;
    }
    
    // Set output parameters based on basin raster
    outSizeX = basinDataset->GetRasterXSize();
    outSizeY = basinDataset->GetRasterYSize();
    outProjWkt = basinDataset->GetProjectionRef();
    if(basinDataset->GetGeoTransform(outGeoTransform) != CE_None) {
        printf("Unable to fetch geographic transform information from raster %s\n", basin_fn);
        return EXIT_FAILURE;
    }
    outPixelHeight = outGeoTransform[5];
    outPixelWidth = outGeoTransform[1];
    
    // Read hillslope raster
    result = _read_raster(hills_fn, &hillsDataset,
                          outSizeX, outSizeY, outPixelHeight, outPixelWidth,
                          outProjWkt);
    if (result != EXIT_SUCCESS) {
        printf("Failed to open raster %s\n", hills_fn);
        return result;
    }
    // Read zone raster
    result = _read_raster(zone_fn, &zoneDataset,
                          outSizeX, outSizeY, outPixelHeight, outPixelWidth,
                          outProjWkt);
    if (result != EXIT_SUCCESS) {
        printf("Failed to open raster %s\n", zone_fn);
        return result;
    }
    // Read patch raster
    result = _read_raster(patch_fn, &patchDataset,
                          outSizeX, outSizeY, outPixelHeight, outPixelWidth,
                          outProjWkt);
    if (result != EXIT_SUCCESS) {
        printf("Failed to open raster %s\n", patch_fn);
        return result;
    }
    
    // Create index of patch IDs
    _index_raster(patch_index, outSizeX, outSizeY,
                  basinDataset, hillsDataset,
                  zoneDataset, patchDataset);
    
    // Create a new dataset for storing output map
    GDALDriver *driver = GetGDALDriverManager()->GetDriverByName("GTiff");
    if (driver == NULL) {
        return EXIT_FAILURE;
    }
    GDALDataset *dest;
    char **papszOptions = NULL;
    papszOptions = CSLSetNameValue(papszOptions, "COMPRESS", "LZW");
    dest = driver->Create(dest_fn, outSizeX, outSizeY, 1, GDT_Float64, papszOptions);
    dest->SetGeoTransform(outGeoTransform);
    dest->SetProjection(outProjWkt);
    GDALRasterBand *destBand = dest->GetRasterBand(1);
    double destNoData = destBand->GetNoDataValue();
    
    // Query data from patchdb
    CassFuture* connect_future = NULL;
    CassCluster* cluster = cass_cluster_new();
    CassSession* session = cass_session_new();
    
    cass_cluster_set_contact_points(cluster, cass_host);
    connect_future = cass_session_connect(session, cluster);
    if (cass_future_error_code(connect_future) == CASS_OK) {
        CassFuture* close_future = NULL;
        
        /* Build statement and execute query */
        // Quick and dirty
        char query[1024];
        snprintf(query, 1024, "SELECT patchid, value "
                               "FROM %s.variables_by_date_patch "
                                "WHERE variable='%s' AND date='%s'",
                 cass_keyspace, variable, date);
        CassStatement* statement = cass_statement_new(query, 0);
        
        CassFuture* result_future = cass_session_execute(session, statement);
        if(cass_future_error_code(result_future) == CASS_OK) {
            /* Retrieve result set and iterate over the rows */
            const CassResult* result = cass_future_get_result(result_future);
            CassIterator* rows = cass_iterator_from_result(result);
            
            while(cass_iterator_next(rows)) {
                const CassRow* row = cass_iterator_get_row(rows);
                const CassValue* value = cass_row_get_column_by_name(row, "patchid");
                
                const char* patchid;
                size_t patchid_length;
                cass_value_get_string(value, &patchid, &patchid_length);
                
                value = cass_row_get_column_by_name(row, "value");
                cass_double_t var_value;
                cass_value_get_double(value, &var_value);
                
                pair<int, int> coord = patch_index[patchid];
                if (raster_index.count(coord.second) == 0) {
                    map<int, double> rowMap;
                    raster_index[coord.second] = rowMap;
                } else {
                    map<int, double> rowMap = raster_index[coord.second];
                }
                raster_index[coord.second][coord.first] = var_value;
            }
            
            cass_result_free(result);
            cass_iterator_free(rows);
        } else {
            /* Handle error */
            const char* message;
            size_t message_length;
            cass_future_error_message(result_future, &message, &message_length);
            fprintf(stderr, "Unable to run query: '%.*s'\n",
                    (int)message_length, message);
            return EXIT_FAILURE;
        }
        
        cass_statement_free(statement);
        cass_future_free(result_future);
        
        /* Close the session */
        close_future = cass_session_close(session);
        cass_future_wait(close_future);
        cass_future_free(close_future);
    } else {
        /* Handle error */
        const char* message;
        size_t message_length;
        cass_future_error_message(connect_future, &message, &message_length);
        fprintf(stderr, "Unable to connect: '%.*s'\n",
                (int)message_length, message);
        return EXIT_FAILURE;
    }
    
    // Write data to output raster
    for (int y = 0; y < outSizeY; y++) {
        for (int x = 0; x < outSizeX; x++) {
            if (raster_index.count(y) != 0) {
                map<int, double> row = raster_index[y];
                double value = destNoData;
                if (row.count(x) != 0) {
                    value = raster_index[y][x];
                }
                // Could do this more efficiently by writing a row at a time,
                // instead of cell-by-cell
                destBand->RasterIO(GF_Write, x, y, 1, 1,
                                   &value, 1, 1, GDT_Float64,
                                   0, 0);
            }
        }
    }
    // Set NODATA value
    destBand->SetNoDataValue(destNoData);
    
    cass_future_free(connect_future);
    cass_cluster_free(cluster);
    cass_session_free(session);
    
    // Clean up
    GDALClose(basinDataset);
    GDALClose(hillsDataset);
    GDALClose(zoneDataset);
    GDALClose(patchDataset);

    if (dest != NULL) {
        GDALClose(dest);
    }
    
    return EXIT_SUCCESS;
}