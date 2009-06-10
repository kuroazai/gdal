/******************************************************************************
 * $Id$
 *
 * Project:  OpenGIS Simple Features Reference Implementation
 * Purpose:  Simple client for translating between formats.
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 *
 ******************************************************************************
 * Copyright (c) 1999, Frank Warmerdam
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 ****************************************************************************/

#include "ogrsf_frmts.h"
#include "ogr_p.h"
#include "cpl_conv.h"
#include "cpl_string.h"
#include "ogr_api.h"
#include "gdal.h"

CPL_CVSID("$Id$");

static void Usage();

static int TranslateLayer( OGRDataSource *poSrcDS, 
                           OGRLayer * poSrcLayer,
                           OGRDataSource *poDstDS,
                           char ** papszLSCO,
                           const char *pszNewLayerName,
                           int bTransform, 
                           OGRSpatialReference *poOutputSRS,
                           OGRSpatialReference *poSourceSRS,
                           char **papszSelFields,
                           int bAppend, int eGType,
                           int bOverwrite,
                           double dfMaxSegmentLength,
                           char** papszFieldTypesToString,
                           long nCountLayerFeatures,
                           GDALProgressFunc pfnProgress,
                           void *pProgressArg);

static int bSkipFailures = FALSE;
static int nGroupTransactions = 200;
static int bPreserveFID = FALSE;
static int nFIDToFetch = OGRNullFID;

/************************************************************************/
/*                                main()                                */
/************************************************************************/

int main( int nArgc, char ** papszArgv )

{
    const char  *pszFormat = "ESRI Shapefile";
    const char  *pszDataSource = NULL;
    const char  *pszDestDataSource = NULL;
    char        **papszLayers = NULL;
    char        **papszDSCO = NULL, **papszLCO = NULL;
    int         bTransform = FALSE;
    int         bAppend = FALSE, bUpdate = FALSE, bOverwrite = FALSE;
    const char  *pszOutputSRSDef = NULL;
    const char  *pszSourceSRSDef = NULL;
    OGRSpatialReference *poOutputSRS = NULL;
    OGRSpatialReference *poSourceSRS = NULL;
    const char  *pszNewLayerName = NULL;
    const char  *pszWHERE = NULL;
    OGRGeometry *poSpatialFilter = NULL;
    const char  *pszSelect;
    char        **papszSelFields = NULL;
    const char  *pszSQLStatement = NULL;
    int         eGType = -2;
    double       dfMaxSegmentLength = 0;
    char        **papszFieldTypesToString = NULL;
    int          bDisplayProgress = FALSE;
    GDALProgressFunc pfnProgress = NULL;
    void        *pProgressArg = NULL;

    /* Check strict compilation and runtime library version as we use C++ API */
    if (! GDAL_CHECK_VERSION(papszArgv[0]))
        exit(1);
/* -------------------------------------------------------------------- */
/*      Register format(s).                                             */
/* -------------------------------------------------------------------- */
    OGRRegisterAll();

/* -------------------------------------------------------------------- */
/*      Processing command line arguments.                              */
/* -------------------------------------------------------------------- */
    nArgc = OGRGeneralCmdLineProcessor( nArgc, &papszArgv, 0 );
    
    if( nArgc < 1 )
        exit( -nArgc );

    for( int iArg = 1; iArg < nArgc; iArg++ )
    {
        if( EQUAL(papszArgv[iArg], "--utility_version") )
        {
            printf("%s was compiled against GDAL %s and is running against GDAL %s\n",
                   papszArgv[0], GDAL_RELEASE_NAME, GDALVersionInfo("RELEASE_NAME"));
            return 0;
        }
        else if( EQUAL(papszArgv[iArg],"-f") && iArg < nArgc-1 )
        {
            pszFormat = papszArgv[++iArg];
        }
        else if( EQUAL(papszArgv[iArg],"-dsco") && iArg < nArgc-1 )
        {
            papszDSCO = CSLAddString(papszDSCO, papszArgv[++iArg] );
        }
        else if( EQUAL(papszArgv[iArg],"-lco") && iArg < nArgc-1 )
        {
            papszLCO = CSLAddString(papszLCO, papszArgv[++iArg] );
        }
        else if( EQUAL(papszArgv[iArg],"-preserve_fid") )
        {
            bPreserveFID = TRUE;
        }
        else if( EQUALN(papszArgv[iArg],"-skip",5) )
        {
            bSkipFailures = TRUE;
            nGroupTransactions = 1; /* #2409 */
        }
        else if( EQUAL(papszArgv[iArg],"-append") )
        {
            bAppend = TRUE;
        }
        else if( EQUAL(papszArgv[iArg],"-overwrite") )
        {
            bOverwrite = TRUE;
        }
        else if( EQUAL(papszArgv[iArg],"-update") )
        {
            bUpdate = TRUE;
        }
        else if( EQUAL(papszArgv[iArg],"-fid") && papszArgv[iArg+1] != NULL )
        {
            nFIDToFetch = atoi(papszArgv[++iArg]);
        }
        else if( EQUAL(papszArgv[iArg],"-sql") && papszArgv[iArg+1] != NULL )
        {
            pszSQLStatement = papszArgv[++iArg];
        }
        else if( EQUAL(papszArgv[iArg],"-nln") && iArg < nArgc-1 )
        {
            pszNewLayerName = papszArgv[++iArg];
        }
        else if( EQUAL(papszArgv[iArg],"-nlt") && iArg < nArgc-1 )
        {
            if( EQUAL(papszArgv[iArg+1],"NONE") )
                eGType = wkbNone;
            else if( EQUAL(papszArgv[iArg+1],"GEOMETRY") )
                eGType = wkbUnknown;
            else if( EQUAL(papszArgv[iArg+1],"POINT") )
                eGType = wkbPoint;
            else if( EQUAL(papszArgv[iArg+1],"LINESTRING") )
                eGType = wkbLineString;
            else if( EQUAL(papszArgv[iArg+1],"POLYGON") )
                eGType = wkbPolygon;
            else if( EQUAL(papszArgv[iArg+1],"GEOMETRYCOLLECTION") )
                eGType = wkbGeometryCollection;
            else if( EQUAL(papszArgv[iArg+1],"MULTIPOINT") )
                eGType = wkbMultiPoint;
            else if( EQUAL(papszArgv[iArg+1],"MULTILINESTRING") )
                eGType = wkbMultiLineString;
            else if( EQUAL(papszArgv[iArg+1],"MULTIPOLYGON") )
                eGType = wkbMultiPolygon;
            else if( EQUAL(papszArgv[iArg+1],"GEOMETRY25D") )
                eGType = wkbUnknown | wkb25DBit;
            else if( EQUAL(papszArgv[iArg+1],"POINT25D") )
                eGType = wkbPoint25D;
            else if( EQUAL(papszArgv[iArg+1],"LINESTRING25D") )
                eGType = wkbLineString25D;
            else if( EQUAL(papszArgv[iArg+1],"POLYGON25D") )
                eGType = wkbPolygon25D;
            else if( EQUAL(papszArgv[iArg+1],"GEOMETRYCOLLECTION25D") )
                eGType = wkbGeometryCollection25D;
            else if( EQUAL(papszArgv[iArg+1],"MULTIPOINT25D") )
                eGType = wkbMultiPoint25D;
            else if( EQUAL(papszArgv[iArg+1],"MULTILINESTRING25D") )
                eGType = wkbMultiLineString25D;
            else if( EQUAL(papszArgv[iArg+1],"MULTIPOLYGON25D") )
                eGType = wkbMultiPolygon25D;
            else
            {
                fprintf( stderr, "-nlt %s: type not recognised.\n", 
                         papszArgv[iArg+1] );
                exit( 1 );
            }
            iArg++;
        }
        else if( (EQUAL(papszArgv[iArg],"-tg") ||
                  EQUAL(papszArgv[iArg],"-gt")) && iArg < nArgc-1 )
        {
            nGroupTransactions = atoi(papszArgv[++iArg]);
        }
        else if( EQUAL(papszArgv[iArg],"-s_srs") && iArg < nArgc-1 )
        {
            pszSourceSRSDef = papszArgv[++iArg];
        }
        else if( EQUAL(papszArgv[iArg],"-a_srs") && iArg < nArgc-1 )
        {
            pszOutputSRSDef = papszArgv[++iArg];
        }
        else if( EQUAL(papszArgv[iArg],"-t_srs") && iArg < nArgc-1 )
        {
            pszOutputSRSDef = papszArgv[++iArg];
            bTransform = TRUE;
        }
        else if( EQUAL(papszArgv[iArg],"-spat") 
                 && papszArgv[iArg+1] != NULL 
                 && papszArgv[iArg+2] != NULL 
                 && papszArgv[iArg+3] != NULL 
                 && papszArgv[iArg+4] != NULL )
        {
            OGRLinearRing  oRing;

            oRing.addPoint( atof(papszArgv[iArg+1]), atof(papszArgv[iArg+2]) );
            oRing.addPoint( atof(papszArgv[iArg+1]), atof(papszArgv[iArg+4]) );
            oRing.addPoint( atof(papszArgv[iArg+3]), atof(papszArgv[iArg+4]) );
            oRing.addPoint( atof(papszArgv[iArg+3]), atof(papszArgv[iArg+2]) );
            oRing.addPoint( atof(papszArgv[iArg+1]), atof(papszArgv[iArg+2]) );

            poSpatialFilter = new OGRPolygon();
            ((OGRPolygon *) poSpatialFilter)->addRing( &oRing );
            iArg += 4;
        }
        else if( EQUAL(papszArgv[iArg],"-where") && papszArgv[iArg+1] != NULL )
        {
            pszWHERE = papszArgv[++iArg];
        }
        else if( EQUAL(papszArgv[iArg],"-select") && papszArgv[iArg+1] != NULL)
        {
            pszSelect = papszArgv[++iArg];
            papszSelFields = CSLTokenizeStringComplex(pszSelect, " ,", 
                                                      FALSE, FALSE );
        }
        else if( EQUAL(papszArgv[iArg],"-segmentize") && iArg < nArgc-1 )
        {
            dfMaxSegmentLength = atof(papszArgv[++iArg]);
        }
        else if( EQUAL(papszArgv[iArg],"-fieldTypeToString") && iArg < nArgc-1 )
        {
            papszFieldTypesToString =
                    CSLTokenizeStringComplex(papszArgv[++iArg], " ,", 
                                             FALSE, FALSE );
            char** iter = papszFieldTypesToString;
            while(*iter)
            {
                if (EQUAL(*iter, "Integer") ||
                    EQUAL(*iter, "Real") ||
                    EQUAL(*iter, "String") ||
                    EQUAL(*iter, "Date") ||
                    EQUAL(*iter, "Time") ||
                    EQUAL(*iter, "DateTime") ||
                    EQUAL(*iter, "Binary") ||
                    EQUAL(*iter, "IntegerList") ||
                    EQUAL(*iter, "RealList") ||
                    EQUAL(*iter, "StringList"))
                {
                    /* Do nothing */
                }
                else if (EQUAL(*iter, "All"))
                {
                    CSLDestroy(papszFieldTypesToString);
                    papszFieldTypesToString = NULL;
                    papszFieldTypesToString = CSLAddString(papszFieldTypesToString, "All");
                    break;
                }
                else
                {
                    fprintf(stderr, "Unhandled type for fieldtypeasstring option : %s\n",
                            *iter);
                    Usage();
                }
                iter ++;
            }
        }
        else if( EQUAL(papszArgv[iArg],"-progress") )
        {
            bDisplayProgress = TRUE;
        }
        else if( papszArgv[iArg][0] == '-' )
        {
            Usage();
        }
        else if( pszDestDataSource == NULL )
            pszDestDataSource = papszArgv[iArg];
        else if( pszDataSource == NULL )
            pszDataSource = papszArgv[iArg];
        else
            papszLayers = CSLAddString( papszLayers, papszArgv[iArg] );
    }

    if( pszDataSource == NULL )
        Usage();

/* -------------------------------------------------------------------- */
/*      Open data source.                                               */
/* -------------------------------------------------------------------- */
    OGRDataSource       *poDS;
        
    poDS = OGRSFDriverRegistrar::Open( pszDataSource, FALSE );

/* -------------------------------------------------------------------- */
/*      Report failure                                                  */
/* -------------------------------------------------------------------- */
    if( poDS == NULL )
    {
        OGRSFDriverRegistrar    *poR = OGRSFDriverRegistrar::GetRegistrar();
        
        fprintf( stderr, "FAILURE:\n"
                "Unable to open datasource `%s' with the following drivers.\n",
                pszDataSource );

        for( int iDriver = 0; iDriver < poR->GetDriverCount(); iDriver++ )
        {
            fprintf( stderr, "  -> %s\n", poR->GetDriver(iDriver)->GetName() );
        }

        exit( 1 );
    }

/* -------------------------------------------------------------------- */
/*      Try opening the output datasource as an existing, writable      */
/* -------------------------------------------------------------------- */
    OGRDataSource       *poODS;
    
    if( bUpdate )
    {
        poODS = OGRSFDriverRegistrar::Open( pszDestDataSource, TRUE );
        if( poODS == NULL )
        {
            fprintf( stderr, "FAILURE:\n"
                    "Unable to open existing output datasource `%s'.\n",
                    pszDestDataSource );
            exit( 1 );
        }

        if( CSLCount(papszDSCO) > 0 )
        {
            fprintf( stderr, "WARNING: Datasource creation options ignored since an existing datasource\n"
                    "         being updated.\n" );
        }
    }

/* -------------------------------------------------------------------- */
/*      Find the output driver.                                         */
/* -------------------------------------------------------------------- */
    else
    {
        OGRSFDriverRegistrar *poR = OGRSFDriverRegistrar::GetRegistrar();
        OGRSFDriver          *poDriver = NULL;
        int                  iDriver;

        for( iDriver = 0;
             iDriver < poR->GetDriverCount() && poDriver == NULL;
             iDriver++ )
        {
            if( EQUAL(poR->GetDriver(iDriver)->GetName(),pszFormat) )
            {
                poDriver = poR->GetDriver(iDriver);
            }
        }

        if( poDriver == NULL )
        {
            fprintf( stderr, "Unable to find driver `%s'.\n", pszFormat );
            fprintf( stderr,  "The following drivers are available:\n" );
        
            for( iDriver = 0; iDriver < poR->GetDriverCount(); iDriver++ )
            {
                fprintf( stderr,  "  -> `%s'\n", poR->GetDriver(iDriver)->GetName() );
            }
            exit( 1 );
        }

        if( !poDriver->TestCapability( ODrCCreateDataSource ) )
        {
            fprintf( stderr,  "%s driver does not support data source creation.\n",
                    pszFormat );
            exit( 1 );
        }

/* -------------------------------------------------------------------- */
/*      Create the output data source.                                  */
/* -------------------------------------------------------------------- */
        poODS = poDriver->CreateDataSource( pszDestDataSource, papszDSCO );
        if( poODS == NULL )
        {
            fprintf( stderr,  "%s driver failed to create %s\n", 
                    pszFormat, pszDestDataSource );
            exit( 1 );
        }
    }

/* -------------------------------------------------------------------- */
/*      Parse the output SRS definition if possible.                    */
/* -------------------------------------------------------------------- */
    if( pszOutputSRSDef != NULL )
    {
        poOutputSRS = new OGRSpatialReference();
        if( poOutputSRS->SetFromUserInput( pszOutputSRSDef ) != OGRERR_NONE )
        {
            fprintf( stderr,  "Failed to process SRS definition: %s\n", 
                    pszOutputSRSDef );
            exit( 1 );
        }
    }

/* -------------------------------------------------------------------- */
/*      Parse the source SRS definition if possible.                    */
/* -------------------------------------------------------------------- */
    if( pszSourceSRSDef != NULL )
    {
        poSourceSRS = new OGRSpatialReference();
        if( poSourceSRS->SetFromUserInput( pszSourceSRSDef ) != OGRERR_NONE )
        {
            fprintf( stderr,  "Failed to process SRS definition: %s\n", 
                    pszSourceSRSDef );
            exit( 1 );
        }
    }

/* -------------------------------------------------------------------- */
/*      Special case for -sql clause.  No source layers required.       */
/* -------------------------------------------------------------------- */
    if( pszSQLStatement != NULL )
    {
        OGRLayer *poResultSet;

        if( pszWHERE != NULL )
            fprintf( stderr,  "-where clause ignored in combination with -sql.\n" );
        if( CSLCount(papszLayers) > 0 )
            fprintf( stderr,  "layer names ignored in combination with -sql.\n" );
        
        poResultSet = poDS->ExecuteSQL( pszSQLStatement, poSpatialFilter, 
                                        NULL );

        if( poResultSet != NULL )
        {
            long nCountLayerFeatures = 0;
            if (bDisplayProgress)
            {
                if (!poResultSet->TestCapability(OLCFastFeatureCount))
                {
                    fprintf( stderr, "Progress turned off as fast feature count is not available.\n");
                    bDisplayProgress = FALSE;
                }
                else
                {
                    nCountLayerFeatures = poResultSet->GetFeatureCount();
                    pfnProgress = GDALTermProgress;
                }
            }

            if( !TranslateLayer( poDS, poResultSet, poODS, papszLCO, 
                                 pszNewLayerName, bTransform, poOutputSRS,
                                 poSourceSRS, papszSelFields, bAppend, eGType,
                                 bOverwrite, dfMaxSegmentLength, papszFieldTypesToString,
                                 nCountLayerFeatures, pfnProgress, pProgressArg))
            {
                CPLError( CE_Failure, CPLE_AppDefined, 
                          "Terminating translation prematurely after failed\n"
                          "translation from sql statement." );

                exit( 1 );
            }
            poDS->ReleaseResultSet( poResultSet );
        }
    }

    else
    {
        int nLayerCount = 0;
        OGRLayer** papoLayers = NULL;

/* -------------------------------------------------------------------- */
/*      Process each data source layer.                                 */
/* -------------------------------------------------------------------- */
        if ( CSLCount(papszLayers) == 0)
        {
            nLayerCount = poDS->GetLayerCount();
            papoLayers = (OGRLayer**)CPLMalloc(sizeof(OGRLayer*) * nLayerCount);

            for( int iLayer = 0; 
                 iLayer < nLayerCount; 
                 iLayer++ )
            {
                OGRLayer        *poLayer = poDS->GetLayer(iLayer);

                if( poLayer == NULL )
                {
                    fprintf( stderr, "FAILURE: Couldn't fetch advertised layer %d!\n",
                            iLayer );
                    exit( 1 );
                }

                papoLayers[iLayer] = poLayer;
            }
        }
/* -------------------------------------------------------------------- */
/*      Process specified data source layers.                           */
/* -------------------------------------------------------------------- */
        else
        {
            nLayerCount = CSLCount(papszLayers);
            papoLayers = (OGRLayer**)CPLMalloc(sizeof(OGRLayer*) * nLayerCount);

            for( int iLayer = 0; 
                papszLayers[iLayer] != NULL; 
                iLayer++ )
            {
                OGRLayer        *poLayer = poDS->GetLayerByName(papszLayers[iLayer]);

                if( poLayer == NULL )
                {
                    fprintf( stderr, "FAILURE: Couldn't fetch advertised layer %s!\n",
                             papszLayers[iLayer] );
                    exit( 1 );
                }

                papoLayers[iLayer] = poLayer;
            }
        }

        long* panLayerCountFeatures = (long*) CPLMalloc(sizeof(long) * nLayerCount);
        long nCountLayersFeatures = 0;
        long nAccCountFeatures = 0;

        /* First pass to apply filters and count all features if necessary */
        for( int iLayer = 0; 
            iLayer < nLayerCount; 
            iLayer++ )
        {
            OGRLayer        *poLayer = papoLayers[iLayer];

            if( pszWHERE != NULL )
                poLayer->SetAttributeFilter( pszWHERE );

            if( poSpatialFilter != NULL )
                poLayer->SetSpatialFilter( poSpatialFilter );

            if (bDisplayProgress)
            {
                if (!poLayer->TestCapability(OLCFastFeatureCount))
                {
                    fprintf( stderr, "Progress turned off as fast feature count is not available.\n");
                    bDisplayProgress = FALSE;
                }
                else
                {
                    panLayerCountFeatures[iLayer] = poLayer->GetFeatureCount();
                    nCountLayersFeatures += panLayerCountFeatures[iLayer];
                }
            }
        }

        /* Second pass to do the real job */
        for( int iLayer = 0; 
            iLayer < nLayerCount; 
            iLayer++ )
        {
            OGRLayer        *poLayer = papoLayers[iLayer];

            if (bDisplayProgress)
            {
                pfnProgress = GDALScaledProgress;
                pProgressArg = 
                    GDALCreateScaledProgress(nAccCountFeatures * 1.0 / nCountLayersFeatures,
                                            (nAccCountFeatures + panLayerCountFeatures[iLayer]) * 1.0 / nCountLayersFeatures,
                                            GDALTermProgress,
                                            NULL);
            }

            nAccCountFeatures += panLayerCountFeatures[iLayer];

            if( !TranslateLayer( poDS, poLayer, poODS, papszLCO, 
                                pszNewLayerName, bTransform, poOutputSRS,
                                poSourceSRS, papszSelFields, bAppend, eGType,
                                bOverwrite, dfMaxSegmentLength, papszFieldTypesToString,
                                panLayerCountFeatures[iLayer], pfnProgress, pProgressArg) 
                && !bSkipFailures )
            {
                CPLError( CE_Failure, CPLE_AppDefined, 
                        "Terminating translation prematurely after failed\n"
                        "translation of layer %s (use -skipfailures to skip errors)\n", 
                        poLayer->GetLayerDefn()->GetName() );

                exit( 1 );
            }

            if (bDisplayProgress)
                GDALDestroyScaledProgress(pProgressArg);
        }

        CPLFree(panLayerCountFeatures);
        CPLFree(papoLayers);
    }

/* -------------------------------------------------------------------- */
/*      Close down.                                                     */
/* -------------------------------------------------------------------- */
    delete poOutputSRS;
    delete poSourceSRS;
    delete poODS;
    delete poDS;

    CSLDestroy(papszSelFields);
    CSLDestroy( papszArgv );
    CSLDestroy( papszLayers );
    CSLDestroy( papszDSCO );
    CSLDestroy( papszLCO );
    CSLDestroy( papszFieldTypesToString );

    OGRCleanupAll();

#ifdef DBMALLOC
    malloc_dump(1);
#endif
    
    return 0;
}

/************************************************************************/
/*                               Usage()                                */
/************************************************************************/

static void Usage()

{
    OGRSFDriverRegistrar        *poR = OGRSFDriverRegistrar::GetRegistrar();

    printf( "Usage: ogr2ogr [--help-general] [-skipfailures] [-append] [-update] [-gt n]\n"
            "               [-select field_list] [-where restricted_where] \n"
            "               [-progress] [-sql <sql statement>] \n" 
            "               [-spat xmin ymin xmax ymax] [-preserve_fid] [-fid FID]\n"
            "               [-a_srs srs_def] [-t_srs srs_def] [-s_srs srs_def]\n"
            "               [-f format_name] [-overwrite] [[-dsco NAME=VALUE] ...]\n"
            "               [-segmentize max_dist] [-fieldTypeToString All|(type1[,type2]*)]\n"
            "               dst_datasource_name src_datasource_name\n"
            "               [-lco NAME=VALUE] [-nln name] [-nlt type] [layer [layer ...]]\n"
            "\n"
            " -f format_name: output file format name, possible values are:\n");
    
    for( int iDriver = 0; iDriver < poR->GetDriverCount(); iDriver++ )
    {
        OGRSFDriver *poDriver = poR->GetDriver(iDriver);

        if( poDriver->TestCapability( ODrCCreateDataSource ) )
            printf( "     -f \"%s\"\n", poDriver->GetName() );
    }

    printf( " -append: Append to existing layer instead of creating new if it exists\n"
            " -overwrite: delete the output layer and recreate it empty\n"
            " -update: Open existing output datasource in update mode\n"
            " -progress: Display progress on terminal. Only works if input layers have the \"fast feature count\" capability\n"
            " -select field_list: Comma-delimited list of fields from input layer to\n"
            "                     copy to the new layer (defaults to all)\n" 
            " -where restricted_where: Attribute query (like SQL WHERE)\n" 
            " -sql statement: Execute given SQL statement and save result.\n"
            " -skipfailures: skip features or layers that fail to convert\n"
            " -gt n: group n features per transaction (default 200)\n"
            " -spat xmin ymin xmax ymax: spatial query extents\n"
            " -segmentize max_dist: maximum distance between 2 nodes.\n"
            "                       Used to create intermediate points\n"
            " -dsco NAME=VALUE: Dataset creation option (format specific)\n"
            " -lco  NAME=VALUE: Layer creation option (format specific)\n"
            " -nln name: Assign an alternate name to the new layer\n"
            " -nlt type: Force a geometry type for new layer.  One of NONE, GEOMETRY,\n"
            "      POINT, LINESTRING, POLYGON, GEOMETRYCOLLECTION, MULTIPOINT,\n"
            "      MULTIPOLYGON, or MULTILINESTRING.  Add \"25D\" for 3D layers.\n"
            "      Default is type of source layer.\n"
            " -fieldTypeToString type1,...: Converts fields of specified types to\n"
            "      fields of type string in the new layer. Valid types are : \n"
            "      Integer, Real, String, Date, Time, DateTime, Binary, IntegerList, RealList,\n"
            "      StringList. Special value All can be used to convert all fields to strings.\n");

    printf(" -a_srs srs_def: Assign an output SRS\n"
           " -t_srs srs_def: Reproject/transform to this SRS on output\n"
           " -s_srs srs_def: Override source SRS\n"
           "\n" 
           " Srs_def can be a full WKT definition (hard to escape properly),\n"
           " or a well known definition (ie. EPSG:4326) or a file with a WKT\n"
           " definition.\n" );

    exit( 1 );
}

/************************************************************************/
/*                           TranslateLayer()                           */
/************************************************************************/

static int TranslateLayer( OGRDataSource *poSrcDS, 
                           OGRLayer * poSrcLayer,
                           OGRDataSource *poDstDS,
                           char **papszLCO,
                           const char *pszNewLayerName,
                           int bTransform, 
                           OGRSpatialReference *poOutputSRS,
                           OGRSpatialReference *poSourceSRS,
                           char **papszSelFields,
                           int bAppend, int eGType, int bOverwrite,
                           double dfMaxSegmentLength,
                           char** papszFieldTypesToString,
                           long nCountLayerFeatures,
                           GDALProgressFunc pfnProgress,
                           void *pProgressArg)

{
    OGRLayer    *poDstLayer;
    OGRFeatureDefn *poFDefn;
    OGRErr      eErr;
    int         bForceToPolygon = FALSE;
    int         bForceToMultiPolygon = FALSE;

    if( pszNewLayerName == NULL )
        pszNewLayerName = poSrcLayer->GetLayerDefn()->GetName();

    if( wkbFlatten(eGType) == wkbPolygon )
        bForceToPolygon = TRUE;
    else if( wkbFlatten(eGType) == wkbMultiPolygon )
        bForceToMultiPolygon = TRUE;

/* -------------------------------------------------------------------- */
/*      Setup coordinate transformation if we need it.                  */
/* -------------------------------------------------------------------- */
    OGRCoordinateTransformation *poCT = NULL;

    if( bTransform )
    {
        if( poSourceSRS == NULL )
            poSourceSRS = poSrcLayer->GetSpatialRef();

        if( poSourceSRS == NULL )
        {
            fprintf( stderr, "Can't transform coordinates, source layer has no\n"
                    "coordinate system.  Use -s_srs to set one.\n" );
            exit( 1 );
        }

        CPLAssert( NULL != poSourceSRS );
        CPLAssert( NULL != poOutputSRS );

        poCT = OGRCreateCoordinateTransformation( poSourceSRS, poOutputSRS );
        if( poCT == NULL )
        {
            char        *pszWKT = NULL;

            fprintf( stderr, "Failed to create coordinate transformation between the\n"
                   "following coordinate systems.  This may be because they\n"
                   "are not transformable, or because projection services\n"
                   "(PROJ.4 DLL/.so) could not be loaded.\n" );
            
            poSourceSRS->exportToPrettyWkt( &pszWKT, FALSE );
            fprintf( stderr,  "Source:\n%s\n", pszWKT );
            
            poOutputSRS->exportToPrettyWkt( &pszWKT, FALSE );
            fprintf( stderr,  "Target:\n%s\n", pszWKT );
            exit( 1 );
        }
    }
    
/* -------------------------------------------------------------------- */
/*      Get other info.                                                 */
/* -------------------------------------------------------------------- */
    poFDefn = poSrcLayer->GetLayerDefn();
    
    if( poOutputSRS == NULL )
        poOutputSRS = poSrcLayer->GetSpatialRef();

/* -------------------------------------------------------------------- */
/*      Find the layer.                                                 */
/* -------------------------------------------------------------------- */
    int iLayer = -1;
    poDstLayer = NULL;

    for( iLayer = 0; iLayer < poDstDS->GetLayerCount(); iLayer++ )
    {
        OGRLayer        *poLayer = poDstDS->GetLayer(iLayer);

        if( poLayer != NULL 
            && EQUAL(poLayer->GetLayerDefn()->GetName(),pszNewLayerName) )
        {
            poDstLayer = poLayer;
            break;
        }
    }
    
/* -------------------------------------------------------------------- */
/*      If the user requested overwrite, and we have the layer in       */
/*      question we need to delete it now so it will get recreated      */
/*      (overwritten).                                                  */
/* -------------------------------------------------------------------- */
    if( poDstLayer != NULL && bOverwrite )
    {
        if( poDstDS->DeleteLayer( iLayer ) != OGRERR_NONE )
        {
            fprintf( stderr, 
                     "DeleteLayer() failed when overwrite requested.\n" );
            return FALSE;
        }
        poDstLayer = NULL;
    }

/* -------------------------------------------------------------------- */
/*      If the layer does not exist, then create it.                    */
/* -------------------------------------------------------------------- */
    if( poDstLayer == NULL )
    {
        if( eGType == -2 )
            eGType = poFDefn->GetGeomType();

        if( !poDstDS->TestCapability( ODsCCreateLayer ) )
        {
            fprintf( stderr, 
              "Layer %s not found, and CreateLayer not supported by driver.", 
                     pszNewLayerName );
            return FALSE;
        }

        CPLErrorReset();

        poDstLayer = poDstDS->CreateLayer( pszNewLayerName, poOutputSRS,
                                           (OGRwkbGeometryType) eGType, 
                                           papszLCO );

        if( poDstLayer == NULL )
            return FALSE;

        bAppend = FALSE;
    }

/* -------------------------------------------------------------------- */
/*      Otherwise we will append to it, if append was requested.        */
/* -------------------------------------------------------------------- */
    else if( !bAppend )
    {
        fprintf( stderr, "FAILED: Layer %s already exists, and -append not specified.\n"
                "        Consider using -append, or -overwrite.\n",
                pszNewLayerName );
        return FALSE;
    }
    else
    {
        if( CSLCount(papszLCO) > 0 )
        {
            fprintf( stderr, "WARNING: Layer creation options ignored since an existing layer is\n"
                    "         being appended to.\n" );
        }
    }

/* -------------------------------------------------------------------- */
/*      Add fields.  Default to copy all field.                         */
/*      If only a subset of all fields requested, then output only      */
/*      the selected fields, and in the order that they were            */
/*      selected.                                                       */
/* -------------------------------------------------------------------- */
    int         iField;

    if (papszSelFields && !bAppend )
    {
        for( iField=0; papszSelFields[iField] != NULL; iField++)
        {
            int iSrcField = poFDefn->GetFieldIndex(papszSelFields[iField]);
            if (iSrcField >= 0)
            {
                if (papszFieldTypesToString != NULL &&
                    (CSLFindString(papszFieldTypesToString, "All") != -1 ||
                     CSLFindString(papszFieldTypesToString,
                                   OGRFieldDefn::GetFieldTypeName(poFDefn->GetFieldDefn(iSrcField)->GetType())) != -1))
                {
                    OGRFieldDefn oFieldDefn( poFDefn->GetFieldDefn(iSrcField) );
                    oFieldDefn.SetType(OFTString);
                    poDstLayer->CreateField( &oFieldDefn );
                }
                else
                    poDstLayer->CreateField( poFDefn->GetFieldDefn(iSrcField) );
            }
            else
            {
                fprintf( stderr, "Field '%s' not found in source layer.\n", 
                        papszSelFields[iField] );
                if( !bSkipFailures )
                    return FALSE;
            }
        }
    }
    else if( !bAppend )
    {
        for( iField = 0; iField < poFDefn->GetFieldCount(); iField++ )
        {
            if (papszFieldTypesToString != NULL &&
                (CSLFindString(papszFieldTypesToString, "All") != -1 ||
                 CSLFindString(papszFieldTypesToString,
                               OGRFieldDefn::GetFieldTypeName(poFDefn->GetFieldDefn(iField)->GetType())) != -1))
            {
                OGRFieldDefn oFieldDefn( poFDefn->GetFieldDefn(iField) );
                oFieldDefn.SetType(OFTString);
                poDstLayer->CreateField( &oFieldDefn );
            }
            else
                poDstLayer->CreateField( poFDefn->GetFieldDefn(iField) );
        }
    }

/* -------------------------------------------------------------------- */
/*      Transfer features.                                              */
/* -------------------------------------------------------------------- */
    OGRFeature  *poFeature;
    int         nFeaturesInTransaction = 0;
    long        nCount = 0;
    
    poSrcLayer->ResetReading();

    if( nGroupTransactions )
        poDstLayer->StartTransaction();

    while( TRUE )
    {
        OGRFeature      *poDstFeature = NULL;

        if( nFIDToFetch != OGRNullFID )
        {
            // Only fetch feature on first pass.
            if( nFeaturesInTransaction == 0 )
                poFeature = poSrcLayer->GetFeature(nFIDToFetch);
            else
                poFeature = NULL;
        }
        else
            poFeature = poSrcLayer->GetNextFeature();
        
        if( poFeature == NULL )
            break;

        if( ++nFeaturesInTransaction == nGroupTransactions )
        {
            poDstLayer->CommitTransaction();
            poDstLayer->StartTransaction();
            nFeaturesInTransaction = 0;
        }

        CPLErrorReset();
        poDstFeature = OGRFeature::CreateFeature( poDstLayer->GetLayerDefn() );

        if( poDstFeature->SetFrom( poFeature, TRUE ) != OGRERR_NONE )
        {
            if( nGroupTransactions )
                poDstLayer->CommitTransaction();
            
            CPLError( CE_Failure, CPLE_AppDefined,
                      "Unable to translate feature %ld from layer %s.\n",
                      poFeature->GetFID(), poFDefn->GetName() );
            
            OGRFeature::DestroyFeature( poFeature );
            OGRFeature::DestroyFeature( poDstFeature );
            return FALSE;
        }

        if( bPreserveFID )
            poDstFeature->SetFID( poFeature->GetFID() );

        OGRGeometry* poDstGeometry = poDstFeature->GetGeometryRef();
        if (poDstGeometry != NULL)
        {
            if (dfMaxSegmentLength > 0)
                poDstGeometry->segmentize(dfMaxSegmentLength);

            if( poCT )
            {
                eErr = poDstGeometry->transform( poCT );
                if( eErr != OGRERR_NONE )
                {
                    if( nGroupTransactions )
                        poDstLayer->CommitTransaction();

                    fprintf( stderr, "Failed to reproject feature %d (geometry probably out of source or destination SRS).\n", 
                            (int) poFeature->GetFID() );
                    if( !bSkipFailures )
                    {
                        OGRFeature::DestroyFeature( poFeature );
                        OGRFeature::DestroyFeature( poDstFeature );
                        return FALSE;
                    }
                }
            }
            else if (poOutputSRS != NULL)
            {
                poDstGeometry->assignSpatialReference(poOutputSRS);
            }

            if( bForceToPolygon )
            {
                poDstFeature->SetGeometryDirectly( 
                    OGRGeometryFactory::forceToPolygon(
                        poDstFeature->StealGeometry() ) );
            }

            if( bForceToMultiPolygon )
            {
                poDstFeature->SetGeometryDirectly( 
                    OGRGeometryFactory::forceToMultiPolygon(
                        poDstFeature->StealGeometry() ) );
            }
        }

        OGRFeature::DestroyFeature( poFeature );

        CPLErrorReset();
        if( poDstLayer->CreateFeature( poDstFeature ) != OGRERR_NONE 
            && !bSkipFailures )
        {
            if( nGroupTransactions )
                poDstLayer->RollbackTransaction();

            OGRFeature::DestroyFeature( poDstFeature );
            return FALSE;
        }

        OGRFeature::DestroyFeature( poDstFeature );

        /* Report progress */
        nCount ++;
        if (pfnProgress)
            pfnProgress(nCount * 1.0 / nCountLayerFeatures, "", pProgressArg);
    }

    if( nGroupTransactions )
        poDstLayer->CommitTransaction();

/* -------------------------------------------------------------------- */
/*      Cleaning                                                        */
/* -------------------------------------------------------------------- */
    delete poCT;

    return TRUE;
}

